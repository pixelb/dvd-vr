//vim:fileencoding=utf8
/*
 dvd-vr.c     Identify and optionally copy the individual programs
              from a DVD-VR format disc

 Copyright © 2007 by Pádraig Brady <P@draigBrady.com>

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

/*

Build:

    gcc -DNDEBUG -std=c99 -Wall -Wpadded dvd-vr.c -o dvd-vr


Usage:

    To get info:   dvd-vr /media/dvd/DVD_RTAV/VR*.IFO
    To rip:        (cd dest-dir && dvd-vr /media/dvd/DVD_RTAV/{*.IFO,*.VRO}


Notes:

    Individual recordings (programs) are extracted,
    honouring any splits and/or deletes.
    Merged programs are not handled yet though as
    I would need to fully parse the higher level program set info.
    Note the VOBs output from this program can be trivially
    concatenated with the unix cat command for example.

    While extracting the DVD data, this program instructs the system
    to not cache the data so that existing cached data is not affected.

    We output the data from this program rather than just outputting offsets
    for use with dd for example, because we may support disjoint VOBUs
    (merged programs) in future. Also in future we may transform the NAV info
    slightly in the VOBs? Anyway it gives us greater control over the system cache
    as described above.

    It might be useful to provide a FUSE module using this logic,
    to present the logical structure of a DVD-VR, maybe even present as DVD-Video?

    Doesn't parse play list index
    Doesn't parse still image info
    Doesn't fixup MPEG time data


Requirements:

    gcc >= 2.95
    glibc >= 2.3.3
    Tested on linux


Changelog:

    V0.1:       16 Feb 2007     Initial release
    V0.2:       29 Mar 2007     Portability fixes
                                  Fixed up comments to support more compilers
                                  Removed linux specific mremap() call
                                  Don't try to use posix_fadvise() where not available
                                  OS X also defines NTOH[LS], so undef first
                                  Use utimes(filename) rather than futimes(fd)
    V0.3:       18 Jun 2007     তন্ময় ভট্টাচার্য্য <tanmoy@mindspring.com>:
                                  Fix random timestamp errors due to uninitialised tm_isdst.
    V0.4:       14 Nov 2007     P@draigBrady.com:
                                  Allow specifiying a particular program to process,
                                  rather than all programs.
                                Aaron Binns <aaron@randomshiznat.com>:
                                  Report 480x480 resolution discs, and do better reporting
                                  for unknown resolutions.
                                Peter Van Hove <Peter@Smart-Projects.net>:
                                  support program label extraction with example code
                                  and nero generated disc images.
                                Craig T. Snydal <ctsnydal@cantab.net>:
                                Rafa Couto <rafacouto@gmail.com>:
                                  Generate globally unique filenames using timestamps,
                                  rather than just program_1.vob, program_2.vob, ...
*/

#define _GNU_SOURCE           /* for posix_fadvise() and futimes() */
#define _FILE_OFFSET_BITS 64  /* for implicit large file support */

#include <inttypes.h>
#include <stdbool.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <locale.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/time.h>

/*********************************************************************************
 *                          support routines
 *********************************************************************************/

#ifndef NDEBUG
void hexdump(const void* data, int len)
{
    int i;
    const unsigned char* bytes=data;
    for (i=0; i<len; i++) {
        printf("%02X ",bytes[i]);
        if ((i+1)%16 ==0) printf ("\n");
    }
    if (len%16) putchar('\n');
}
#endif//NDEBUG

typedef enum {
    PERCENT_START,
    PERCENT_UPDATE,
    PERCENT_END
} percent_control_t;

void percent_display(percent_control_t percent_control, unsigned int percent)
{
    static int point;
    #define POINTS 20

    switch (percent_control) {
    case PERCENT_START: {
        point=0;
        printf("[%*s]\r[",POINTS,"");
        break;
    }
    case PERCENT_UPDATE: {
        int newpoint=percent/(100/POINTS);
        int i;
        for (i=0; i<newpoint-point; i++)
            putchar('.');
        point=newpoint;
        break;
    }
    case PERCENT_END: {
        printf("\r %*s \r",POINTS,"");
        break;
    }
    }
    fflush(stdout);
}

/* Set access and modfied times of filename
   to the specified broken down time */
int touch(const char* filename, struct tm* tm)
{
    time_t ut = mktime(tm);
    struct timeval tv[2]={ {.tv_sec=ut, .tv_usec=0}, {.tv_sec=ut, .tv_usec=0} };
    return utimes(filename, tv);
}

/*
  Copy data between file descriptors while not
  putting more than blocks*block_size in the system cache.
  Therefore you will probably want to call this function repeatedly.
 */
int stream_data(int src_fd, int dst_fd, uint32_t blocks, uint16_t block_size) {
    /* TODO: mmap/madvise instead of read/fadvise ? What about splice? */

    uint8_t buf[block_size];
    int block;
    for (block=0; block<blocks; block++) {
        if (read(src_fd,buf,sizeof(buf)) != sizeof(buf)) {
            fprintf(stderr,"Error reading from SRC [%m]\n");
            return -1;
        }
        if (write(dst_fd,buf,sizeof(buf)) != sizeof(buf)) {
            fprintf(stderr,"Error writing to DST [%m]\n");
            return -1;
        }
    }

#ifdef POSIX_FADV_DONTNEED
    /* Don't fill cache with SRC.
    Note be careful to invalidate only what we've written
    so that we don't dump any readahead cache. */
    uint32_t bytes=blocks*block_size;
    off_t offset=lseek(src_fd,0,SEEK_CUR);
    posix_fadvise(src_fd, offset-bytes, bytes, POSIX_FADV_DONTNEED);

    /* Don't fill cache with DST.
    Note this slows the operation down by 20% when both source
    and dest are on the same hard disk at least. I guess
    this is due to implicit syncing in posix_fadvise()? */
    posix_fadvise(dst_fd, 0, 0, POSIX_FADV_DONTNEED);
#endif //POSIX_FADV_DONTNEED

    return 0;
}

/*********************************************************************************
 *                          The DVD-VR structures
 *********************************************************************************/

#undef PACKED
#if defined(__GNUC__)
# if __GNUC__ > 2 || (__GNUC__ == 2 && __GNUC_MINOR__ >= 95)
#  define PACKED __attribute__ ((packed))
# endif
#endif
#if !defined(PACKED)
# error "Your compiler doesn't support __attribute__ ((packed))"
#endif

/* DVD structures are in network byte order (big endian) */
#include <netinet/in.h>
#undef NTOHS
#undef NTOHL
#define NTOHS(x) x=ntohs(x) /* 16 bit */
#define NTOHL(x) x=ntohl(x) /* 32 bit */

#define DVD_SECTOR_SIZE 2048

typedef struct {
    struct {
        /* 0 */
        char     id[12];
        uint32_t vmg_ea;      /*end address*/
        uint8_t  zero1[12];
        uint32_t vmgi_ea;     /*includes playlist info after this structure*/
        uint16_t version;     /*specification version*/
        /* 34. Different from DVD-Video from here */
        uint8_t  zero2[30];
        uint8_t  data[34];
        uint8_t  zero3[158];
        /* 256 */
        uint32_t pgit_sa;    /*program info table start address*/
        uint32_t info2_sa;   /*another start address*/
        uint8_t  zero4[40];
        uint32_t def_psi_sa; /*default program set info start address*/
        uint32_t info4_sa;   /*and another*/
        uint32_t info5_sa;   /*and another (user defined program set info?)*/
        uint32_t info6_sa;   /*and another*/
        uint8_t  zero5[192];
    } PACKED mat;
} PACKED rtav_vmgi_t; /*Real Time AV (from DVD_RTAV dir)*/

typedef struct {
    uint8_t audio_attr[3];
} PACKED audio_attr_t;

typedef struct {
    uint8_t pgtm[5];
} PACKED pgtm_t;

typedef struct {
    uint32_t ptm;
    uint16_t ptm_extra; /* extra to DSI pkts */
} PACKED ptm_t;

typedef struct {
    uint16_t vob_attr;
    pgtm_t   vob_timestamp;
    uint8_t  data1;
    uint8_t  vob_format_id;
    ptm_t    vob_v_s_ptm;
    ptm_t    vob_v_e_ptm;
} PACKED vvob_t; /* Virtual VOB */
typedef struct {
    uint8_t  data[12];
} PACKED adj_vob_t;
typedef struct {
    uint16_t nr_of_time_info;
    uint16_t nr_of_vobu_info;
    uint16_t time_offset;
    uint32_t vob_offset;
} PACKED vobu_map_t;
typedef struct {
    uint8_t  data[7];
} PACKED time_info_t;
typedef struct {
    uint8_t vobu_info[3];
} PACKED vobu_info_t;

typedef struct {
    uint16_t zero1;
    uint8_t  nr_of_pgi;
    uint8_t  nr_of_vob_formats;
    uint32_t pgit_ea;
} PACKED pgiti_t; /* info for ProGram Info Table */

typedef struct {
    uint16_t video_attr;
    uint8_t  nr_of_audio_streams;
    uint8_t  data1;
    audio_attr_t  audio_attr0;
    audio_attr_t  audio_attr1;
    uint8_t  data2[50];
} PACKED vob_format_t;

typedef struct {
    uint16_t nr_of_programs;
} PACKED pgi_gi_t; /* global info for ProGram Info */

typedef struct  {
    uint8_t  data1;
    uint8_t  nr_of_psi;
    uint16_t nr_of_programs;  /* Num programs on disc */
} PACKED psi_gi_t; /* global info for Program Set Info */

typedef struct  {
    uint8_t  data1;
    uint8_t  data2;
    uint16_t nr_of_programs;  /* Num programs in program set */
    char     label[128];      /* contains ascii at start */
    uint16_t data3;
    uint16_t id;              /* corresponding program set number */
    char     data4[6];
} PACKED psi_t;

bool parse_audio_attr(audio_attr_t audio_attr0)
{
    int coding   = (audio_attr0.audio_attr[0] & 0xE0)>>5;
    int channels = (audio_attr0.audio_attr[1] & 0x0F);
    /* audio_attr0.audio_attr[2] = 7 for my camcorder ? */

    if (channels < 8) {
        printf ("audio_channs: %d\n",channels+1);
    } else {
        return false;
    }

    const char* coding_name="Unknown";
    switch (coding) {
    case 0: coding_name="Dolby AC-3"; break;
    case 2: coding_name="MPEG-1"; break;
    case 3: coding_name="MPEG-2ext"; break;
    case 4: coding_name="Linear PCM"; break;
    }
    printf("audio_coding: %s",coding_name);
    if (!strcmp("Unknown", coding_name)) {
        printf( ". (%d). Please report this number and actual audio encoding.\n", coding );
    } else {
        putchar('\n');
    }

    return true;
}

bool parse_video_attr(uint16_t video_attr)
{
    int resolution  = (video_attr & 0x0038) >>  3;
    int aspect      = (video_attr & 0x0C00) >> 10;
    int tv_sys      = (video_attr & 0x3000) >> 12;
    int compression = (video_attr & 0xC000) >> 14;

    int vert_resolution  = 0;
    int horiz_resolution = 0;
    switch (tv_sys) {
    case 0:
        printf( "tv_system   : NTSC\n" );
        vert_resolution=480;
        break;
    case 1:
        printf( "tv_system   : PAL\n" );
        vert_resolution=576;
        break;
    default:
        return false;
    }

    switch (resolution) {
    case 0: horiz_resolution=720; break;
    case 1: horiz_resolution=704; break;
    case 2: horiz_resolution=352; break;
    case 3: horiz_resolution=352; vert_resolution/=2; break;
    case 4: horiz_resolution=544; break; /* this is a google inspired guess */
    case 5: horiz_resolution=480; break; /* from Aaron Binns' disc */
    }
    if (horiz_resolution && vert_resolution) {
        printf( "resolution  : %dx%d\n", horiz_resolution, vert_resolution);
    } else {
        printf( "resolution  : Unknown (%d). Please report this number and actual resolution.\n", resolution );
    }

    const char* mode = "Unknown";
    switch (compression) {
    case 0: mode="MPEG1"; break;
    case 1: mode="MPEG2"; break;
    }
    printf( "video_format: %s", mode );
    if (!strcmp("Unknown", mode)) {
        printf( ". (%d). Please report this number and actual compression format.\n", compression );
    } else {
        putchar('\n');
    }

    const char* aspect_ratio = "Unknown";
    switch (aspect) {
    case 0: aspect_ratio="4:3"; break;
    case 1: aspect_ratio="16:9"; break; /* This is 3 for DVD-Video */
    }
    printf( "aspect_ratio: %s", aspect_ratio );
    if (!strcmp("Unknown", aspect_ratio)) {
        printf( ". (%d). Please report this number and actual aspect ratio.\n", aspect );
    } else {
        putchar('\n');
    }

    return true;
}

bool parse_pgtm(pgtm_t pgtm, struct tm* tm)
{
    bool ret=false;

    uint16_t year  = ((pgtm.pgtm[0]       ) <<8 | (pgtm.pgtm[1]     )) >> 2;
    uint8_t  month =  (pgtm.pgtm[1] & 0x03) <<2 | (pgtm.pgtm[2] >> 6);
    uint8_t  day   =  (pgtm.pgtm[2] & 0x3E) >>1;
    uint8_t  hour  =  (pgtm.pgtm[2] & 0x01) <<4 | (pgtm.pgtm[3] >> 4);
    uint8_t  min   =  (pgtm.pgtm[3] & 0x0F) <<2 | (pgtm.pgtm[4] >> 6);
    uint8_t  sec   =  (pgtm.pgtm[4] & 0x3F);
    if (year) {
        tm->tm_year=year-1900;
        tm->tm_mon=month-1;
        tm->tm_mday=day;
        tm->tm_hour=hour;
        tm->tm_min=min;
        tm->tm_sec=sec;
        tm->tm_isdst=-1; /*Auto calc DST offset.*/

        char date_str[32];
        strftime(date_str,sizeof(date_str),"%F %T",tm); //locale = %x %X
        printf("date : %s\n",date_str);
        ret=true;
    } else {
        printf("date : not set\n");
    }
    return ret;
}

/*
 * XXX: This assumes the programs occur linearly within
 * the default program sets. This is accurate at least for
 * my camcorder, which creates a program set per day, and
 * auto sets the label to the date (which can be relabled).
 * This is also valid fot the LG camcorder and Nero generated
 * DVD-VR images I was sent, which create a program set for
 * each program.
 */
char* find_program_text_info(psi_gi_t* psi_gi, int program)
{
    int ps;
    for (ps=0; ps<psi_gi->nr_of_psi; ps++) {
        psi_t *psi = (psi_t*)(((char*)(psi_gi+1)) + (ps * sizeof(psi_t)));
        uint16_t start_prog_num = ntohs(psi->id);
        uint16_t end_prog_num = start_prog_num + ntohs(psi->nr_of_programs) - 1;
        if ((program >= start_prog_num) && (program <= end_prog_num)) {
            return psi->label;
        }
    }
    return "Couldn't find program label. Please report";
}

/*********************************************************************************
 *
 *********************************************************************************/

unsigned required_program=0; /* process all programs by default */
const char* ifo_name=NULL;
const char* vro_name=NULL;

void usage(char** argv)
{
    fprintf(stderr,"Usage: %s [-p #] VR_MANGR.IFO [VR_MOVIE.VRO]\n"
                    "\n"
                    "If -p # is specified, then only that program is processed\n"
                    "If the VRO file is specified, the component programs are\n"
                    "extracted to the current directory\n",argv[0]);
    exit(EXIT_FAILURE);
}

void get_options(int argc, char** argv)
{
    int opt;
    while ((opt = getopt(argc, argv, "p:")) != -1) {
        switch (opt) {
        case 'p':
            required_program = atoi(optarg);
            break;
        default: /* '?' */
            usage(argv);
            break;
        }
    }
    if (optind >= argc) {
        usage(argv);
    }
    ifo_name=argv[optind++];

    if (optind < argc) {
        vro_name=argv[optind++];
    }
}

int main(int argc, char** argv)
{
    setlocale(LC_ALL,"");

    get_options(argc, argv);

    int fd=open(ifo_name,O_RDONLY);
    if (fd == -1) {
        fprintf(stderr,"Error opening [%s] (%m)\n",ifo_name);
        exit(EXIT_FAILURE);
    }

    int vro_fd=-1;
    if (vro_name) {
        vro_fd=open(vro_name,O_RDONLY);
        if (vro_fd == -1) {
            fprintf(stderr,"Error opening [%s] (%m)\n",vro_name);
            exit(EXIT_FAILURE);
        }
#ifdef POSIX_FADV_SEQUENTIAL
        posix_fadvise(vro_fd, 0, 0, POSIX_FADV_SEQUENTIAL);/* More readahead done */
#endif //POSIX_FADV_SEQUENTIAL
    }

    rtav_vmgi_t* rtav_vmgi_ptr=mmap(0,sizeof(rtav_vmgi_t),PROT_READ|PROT_WRITE,MAP_PRIVATE,fd,0);
    if (rtav_vmgi_ptr == MAP_FAILED) {
        fprintf(stderr,"Failed to MMAP ifo file (%m)\n");
        exit(EXIT_FAILURE);
    }
    if (strncmp("DVD_RTR_VMG0",rtav_vmgi_ptr->mat.id,sizeof(rtav_vmgi_ptr->mat.id))) {
        fprintf(stderr,"invalid DVD-VR IFO identifier\n");
        exit(EXIT_FAILURE);
    }

    uint32_t vmg_size = NTOHL(rtav_vmgi_ptr->mat.vmg_ea) + 1;
    if (munmap(rtav_vmgi_ptr, sizeof(rtav_vmgi_t)!=0)) {
        fprintf(stderr,"Failed to unmap ifo file (%m)\n");
        exit(EXIT_FAILURE);
    }
    rtav_vmgi_ptr=mmap(0,vmg_size,PROT_READ|PROT_WRITE,MAP_PRIVATE,fd,0);
    if (rtav_vmgi_ptr == MAP_FAILED) {
        fprintf(stderr,"Failed to re MMAP ifo file (%m)\n");
        exit(EXIT_FAILURE);
    }

    NTOHS(rtav_vmgi_ptr->mat.version);
    rtav_vmgi_ptr->mat.version &= 0x00FF;
    printf("format: DVD-VR V%d.%d\n",rtav_vmgi_ptr->mat.version>>4,rtav_vmgi_ptr->mat.version&0x0F);

    NTOHL(rtav_vmgi_ptr->mat.pgit_sa);
    pgiti_t* pgiti = (pgiti_t*) ((char*)rtav_vmgi_ptr + rtav_vmgi_ptr->mat.pgit_sa);
    NTOHL(pgiti->pgit_ea);

    NTOHL(rtav_vmgi_ptr->mat.def_psi_sa);
    psi_gi_t *def_psi_gi = (psi_gi_t*) ((char*)rtav_vmgi_ptr + rtav_vmgi_ptr->mat.def_psi_sa);

#ifndef NDEBUG
    printf("Number of info tables for VRO: %d\n",pgiti->nr_of_pgi);
    printf("Number of vob formats: %d\n",pgiti->nr_of_vob_formats);
    printf("pgit_ea: %08X\n",pgiti->pgit_ea);
#endif//NDEBUG

    if (pgiti->nr_of_pgi == 0) {
        fprintf(stderr,"Error, couldn't find info table for VRO\n");
        exit(EXIT_FAILURE);
    }
    if (pgiti->nr_of_pgi > 1) {
        fprintf(stderr,"Warning, Only processing 1 of the %"PRIu8" VRO info tables\n",pgiti->nr_of_pgi);
    }

    vob_format_t* vob_format = (vob_format_t*) (pgiti+1);
    int vob_type;
    int vob_types=pgiti->nr_of_vob_formats;
    for (vob_type=0; vob_type<vob_types; vob_type++) {
        putchar('\n');
        if (vob_types>1) {
            printf("VOB format %d...\n",vob_type+1);
        }
        NTOHS(vob_format->video_attr);
        if (!parse_video_attr(vob_format->video_attr)) {
            fprintf(stderr,"Error parsing video_attr\n");
        }
        if (!parse_audio_attr(vob_format->audio_attr0)) {
            fprintf(stderr,"Error parsing audio_attr0\n");
        }
        vob_format++;
    }

    pgi_gi_t* pgi_gi = (pgi_gi_t*) vob_format;
    NTOHS(pgi_gi->nr_of_programs);
    printf("\nNumber of programs: %d\n", pgi_gi->nr_of_programs);
    if (required_program && required_program>pgi_gi->nr_of_programs) {
        fprintf(stderr,"Error: couldn't find specified program (%d)\n", required_program);
        exit(EXIT_FAILURE);
    }

    struct tm now_tm;
    time_t now=time(0);
    (void) gmtime_r(&now, &now_tm);//used if no timestamp in program
    int program;
    typedef uint32_t vvobi_sa_t;
    vvobi_sa_t* vvobi_sa=(vvobi_sa_t*)(pgi_gi+1);
    for (program=0; program<pgi_gi->nr_of_programs; program++) {

        if (required_program && program+1!=required_program) {
            vvobi_sa++;
            continue;
        }

        NTOHL(*vvobi_sa);

        putchar('\n');
#ifndef NDEBUG
        printf("VVOB info (%d) address: %d\n",program+1,*vvobi_sa);
#endif//NDEBUG
        vvob_t* vvob = (vvob_t*) (((uint8_t*)pgiti) + *vvobi_sa);
        struct tm tm;
        char date_str[32]; //use date to give unique filename
        if (parse_pgtm(vvob->vob_timestamp,&tm)) {
            strftime(date_str,sizeof(date_str),"%F_%T",&tm);
        } else { //use now + program num to give unique name
            strftime(date_str,sizeof(date_str),"%F_%T",&now_tm);
            int datelen=strlen(date_str);
            (void) snprintf(date_str+datelen, sizeof(date_str)-datelen, "_%d", program+1);
        }

        int vob_fd=-1;
        char vob_name[32];
        if (vro_fd!=-1) {
            (void) snprintf(vob_name,sizeof(vob_name),"%s.vob",date_str);
            vob_fd=open(vob_name,O_WRONLY|O_CREAT|O_EXCL,0666);
            if (vob_fd == -1) {
                fprintf(stderr,"Error opening [%s] (%m)\n",vob_name);
                exit(EXIT_FAILURE);
            }
        }

        if (vob_types>1) {
            printf("vob format: %d\n", vvob->vob_format_id);
        }
        NTOHS(vvob->vob_attr);
        int skip=0;
        if (vvob->vob_attr & 0x80) {
            skip+=sizeof(adj_vob_t);
#ifndef NDEBUG
            printf("skipping adjacent VOB info\n");
#endif//NDEBUG
        }
        skip+=sizeof(uint16_t); /* ?? */
        vobu_map_t* vobu_map = (vobu_map_t*) (((uint8_t*)(vvob+1)) + skip);
        NTOHS(vobu_map->nr_of_time_info);
        NTOHS(vobu_map->nr_of_vobu_info);
        NTOHS(vobu_map->time_offset);
        NTOHL(vobu_map->vob_offset);
#ifndef NDEBUG
        printf("num time infos:   %"PRIu16"\n",vobu_map->nr_of_time_info);
        printf("num VOBUs: %"PRIu16"\n",vobu_map->nr_of_vobu_info);
        printf("time offset:      %"PRIu16"\n",vobu_map->time_offset); /* What units? */
        printf("vob offset:     %"PRIu32"*%d\n",vobu_map->vob_offset,DVD_SECTOR_SIZE);  /* offset in the VRO file of the VOB */
#endif//NDEBUG
        if (vro_fd!=-1) {
            if (lseek(vro_fd, vobu_map->vob_offset*DVD_SECTOR_SIZE, SEEK_SET)==(off_t)-1) {
                fprintf(stderr,"Error seeking within VRO [%m]\n");
                exit(EXIT_FAILURE);
            }
        }
        vobu_info_t* vobu_info = (vobu_info_t*) (((uint8_t*)(vobu_map+1)) + vobu_map->nr_of_time_info*sizeof(time_info_t));
        int vobus;
        uint64_t tot=0;
        if (vro_fd != -1) {
            percent_display(PERCENT_START,0);
        }
        for (vobus=0; vobus<vobu_map->nr_of_vobu_info; vobus++) {
            uint16_t vobu_size = *(uint16_t*)(&vobu_info->vobu_info[1]);
            NTOHS(vobu_size); vobu_size&=0x03FF;
            if (vro_fd != -1) {
                if (stream_data(vro_fd, vob_fd, vobu_size, DVD_SECTOR_SIZE) == -1) {
                    exit(EXIT_FAILURE);
                }

                int percent=((vobus+1)*100)/vobu_map->nr_of_vobu_info;
                percent_display(PERCENT_UPDATE, percent);
            }
            tot+=vobu_size;
            vobu_info++;
        }
        if (vro_fd != -1) {
            percent_display(PERCENT_END, 0);
            close(vob_fd);
            touch(vob_name, &tm);
        }

        printf("size : %'"PRIu64"\n",tot*DVD_SECTOR_SIZE);
        printf("label: %s\n", find_program_text_info(def_psi_gi, program+1));

        vvobi_sa++;
    }

    munmap(rtav_vmgi_ptr, vmg_size);
    close(fd);
    if (vro_fd != -1)
        close(vro_fd);

    return EXIT_SUCCESS;
}
