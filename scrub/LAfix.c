
/*
    repairs gaps and weak regions based on a read's overlaps
    and produces a new set of sequences

    Author: MARVEL Team
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <sys/param.h>

#include "lib/colors.h"
#include "lib/tracks.h"
#include "lib/pass.h"
#include "lib/oflags.h"
#include "lib/utils.h"

#include "db/DB.h"
#include "dalign/align.h"

// defaults

#define DEF_ARG_X    1000
#define DEF_ARG_G     500   // don't patch gaps above a certain size
#define DEF_ARG_Q      28   // low quality cutoff

// settings

#define FASTA_WIDTH    60   // wrapping for result fasta files

#define MIN_INT_LEN     5   // min length of an adjusted track interval

#define MIN_SPAN      400   // only alignments with at least MIN_SPAN bases left and right of a segment are considering as support

// toggles

#undef DEBUG
#undef DEBUG_INTERVAL_ADJUSTMENT
#undef DEBUG_FLIP

#undef VERBOSE

// for getopt()

extern char* optarg;
extern int optind, opterr, optopt;

// context

typedef struct
{
    HITS_DB* db;
    int twidth;

    FILE* fileFastaOut;
    FILE* fileQvOut;

    // arguments

    int minlen;
    int lowq;
    int maxgap;
    int trim;

    HITS_TRACK* qtrack;
    char *trimName;
    HITS_TRACK* trimtrack;
    HITS_TRACK* dusttrack;

    HITS_TRACK** convertTracks;
    int curctracks;
    int maxctracks;

    uint64_t stats_bases_before;      // bases before patching
    uint64_t stats_bases_after;       // bases after patching

    // stats

    int num_flips;
    int num_gaps;

    char* reada;
    char* readb;
    char* read_patched;

    char** qva;
    char** qvb;
    char** qv_patched;

    int* apatches;

} FixContext;

// information on a gap/weak region

typedef struct
{
    int ab;         // a begin
    int ae;         // a end

    int bb;         // b begin
    int be;         // b end

    int diff;       // quality
    int b;          // b read id
    int support;    // how many reads support the gap
    int span;       // reads spanning the gap

    int comp;       // complement sequence when writing
} Gap;

static void fix_pre(PassContext* pctx, FixContext* fctx)
{
#ifdef VERBOSE
    printf(ANSI_COLOR_GREEN "PASS fix" ANSI_COLOR_RESET "\n");
#endif

    fctx->twidth = pctx->twidth;

    if ( !(fctx->qtrack = track_load(fctx->db, TRACK_Q)) )
    {
        fprintf(stderr, "failed to open track %s\n", TRACK_Q);
        exit(1);
    }

    if (fctx->trimName)
    {
        if ( !(fctx->trimtrack = track_load(fctx->db, fctx->trimName)) )
        {
            fprintf(stderr, "failed to open track %s\n", fctx->trimName);
            exit(1);
        }
    }
    else
    {
        fctx->trimtrack = NULL;
    }

    if ( !(fctx->dusttrack = track_load(fctx->db, TRACK_DUST)) )
    {
        fprintf(stderr, "failed to open track %s\n", TRACK_DUST);
        exit(1);
    }

    int maxlen = fctx->db->maxlen;

    fctx->reada = New_Read_Buffer(fctx->db);
    fctx->readb = New_Read_Buffer(fctx->db);
    fctx->read_patched = malloc(maxlen * 2 + 4);

    if (fctx->fileQvOut)
    {
        fctx->qva = New_QV_Buffer(fctx->db);
        fctx->qvb = New_QV_Buffer(fctx->db);
        fctx->qv_patched = malloc(sizeof(char*) * NUM_QV_STREAMS);

        char* qvs = malloc( maxlen * 2 * NUM_QV_STREAMS );
        int i;
        for (i = 0; i < NUM_QV_STREAMS; i++)
        {
            fctx->qv_patched[i] = qvs + i * maxlen * 2;
        }
    }

    fctx->apatches = malloc( (maxlen / pctx->twidth + 1) * 3 * sizeof(int) );
}

static void fix_post(PassContext* pctx, FixContext* fctx)
{
#ifdef VERBOSE
    printf("gaps: %d\n", fctx->num_gaps);
    printf("flips: %d\n", fctx->num_flips);
    printf("replaced %'" PRIu64 "with %'" PRIu64 " bases\n", fctx->stats_bases_before, fctx->stats_bases_after);
#endif

    UNUSED(pctx);

    free(fctx->reada - 1);
    free(fctx->readb - 1);
    free(fctx->read_patched);

    if (fctx->fileQvOut)
    {
        Free_QV_Buffer(fctx->qva);
        Free_QV_Buffer(fctx->qvb);
        Free_QV_Buffer(fctx->qv_patched);
    }

    free(fctx->apatches);
}

static int cmp_gaps(const void* x, const void* y)
{
    Gap* a = (Gap*)x;
    Gap* b = (Gap*)y;

    int cmp = a->ab - b->ab;

    if (cmp == 0)
    {
        cmp = a->ae - b->ae;
    }

    if (cmp == 0)
    {
        cmp = a->diff - b->diff;
    }

    return cmp;
}

static int spanners(Overlap* ovls, int novl, int b, int e)
{
    int span = 0;
    int i;
    for (i = 0; i < novl; i++)
    {
        Overlap* ovl = ovls + i;

        if (ovl->path.abpos < b - MIN_SPAN && ovl->path.aepos > e + MIN_SPAN)
        {
            span++;
        }
    }

    return span;
}

static int filter_flips(FixContext* fctx, Overlap* ovls, int novl, int* trim_b, int* trim_e)
{
    int self_n = 0;
    int self_c = 0;
    int aread = ovls->aread;
    int trimmed = 0;

    int b = -1;
    int e = -1;

    int i;
    for (i = 0; i < novl; i++)
    {
        Overlap* ovl = ovls + i;
        int bread = ovl->bread;

        if (aread < bread)
        {
            if (b != -1)
            {
                e = i;
            }

            break;
        }
        else if (aread == bread)
        {
            if (b == -1)
            {
                b = i;
            }

            if (ovl->flags & OVL_COMP)
            {
                self_c++;
            }
            else
            {
                self_n++;
            }
        }
    }

    if (self_c == 0)
    {
        return trimmed;
    }

    // printf("%7d %3d %3d [%4d..%4d]\n", aread, self_c, self_n, b, e);

    int alen = DB_READ_LEN(fctx->db, aread);

    for (i = b ; i < e ; i++)
    {
        Overlap* ovl = ovls + i;

        if (ovl->flags & OVL_COMP)
        {
            int ab = ovl->path.abpos;
            int ae = ovl->path.aepos;

            int ab_c = alen - ovl->path.bepos;
            int ae_c = alen - ovl->path.bbpos;

            if (intersect(ab, ae, ab_c, ae_c))
            {
                // printf("  -> crosses diagonal %5d..%5d x %5d..%5d\n", ab, ae, ab_c, ae_c);

                ovl_trace* trace = ovl->path.trace;

                int sab = ovl->path.abpos;
                int sae = (sab / fctx->twidth + 1) * fctx->twidth;
                int sbb = ovl->path.bbpos;
                int sbe = sbb + trace[1];

                int j;
                for (j = 2; j < ovl->path.tlen - 2; j += 2)
                {
                    if ( intersect(sab, sae, alen - sbe, alen - sbb) ) // && spanners(ovls, novl, sab, sae) <= 1 )
                    {
#ifdef DEBUG_FLIP
                        printf("%8d CROSS @ %5d..%5d x %5d..%5d\n",
                        aread, sab, sae, alen - sbe, alen - sbb);
#endif
                        trimmed = 1;

                        if (*trim_b < sab && sae < *trim_e)
                        {
                            if (sab - *trim_b < *trim_e - sae)
                            {
                                *trim_b = sae;
                            }
                            else
                            {
                                *trim_e = sab;
                            }
                        }
                    }

                    sab = sae;
                    sae += fctx->twidth;

                    sbb = sbe;
                    sbe += trace[j + 1];
                }

                sae = ovl->path.aepos;
                sbe = ovl->path.bepos;
            }
        }
    }

    i = b;
    while (i < e - 1)
    {
        Overlap* ovl = ovls + i;
        Overlap* ovl2 = ovls + i + 1;

        if ( (ovl->flags & OVL_COMP) && (ovl2->flags & OVL_COMP) )
        {
            int ab = ovl->path.aepos;
            int ae = ovl2->path.abpos;
            int ab_c = alen - ovl2->path.bbpos;
            int ae_c = alen - ovl->path.bepos;

            if (intersect(ab, ae, ab_c, ae_c) && spanners(ovls, novl, ab, ae) <= 1)
            {
#ifdef DEBUG_FLIP
                printf("%8d GAP   @ %5d..%5d x %5d..%5d\n",
                        aread, ab, ae, ab_c, ae_c);
#endif
                trimmed = 1;

                int mid = (ab + ae) / 2;

                if (*trim_b < mid && mid < *trim_e)
                {
                    if (mid - *trim_b < *trim_e - mid)
                    {
                        *trim_b = mid;
                    }
                    else
                    {
                        *trim_e = mid;
                    }
                }
            }
        }

        i += 1;
    }

    return trimmed;
}

static int fix_process(void* _ctx, Overlap* ovl, int novl)
{
// #warning "REMOVE ME"
//    if ( ovl->aread != 229 ) return 1;

    FixContext* fctx = (FixContext*)_ctx;

    int maxgap = fctx->maxgap;
    int lowq = fctx->lowq;

    int dcur = 0;
    int dmax = 1000;
    Gap* data = malloc(sizeof(Gap) * dmax);

    track_anno* qanno = fctx->qtrack->anno;
    track_data* qdata = fctx->qtrack->data;

    track_anno* dustanno = fctx->dusttrack->anno;
    track_data* dustdata = fctx->dusttrack->data;

    track_data* qa = qdata + (qanno[ovl->aread] / sizeof(track_data));

    // get trim offsets and skip reads that get trimmed away

    int trim_ab, trim_ae;
    if (fctx->trimtrack)
    {
        get_trim(fctx->db, fctx->trimtrack, ovl->aread, &trim_ab, &trim_ae);
    }
    else
    {
        trim_ab = 0;
        trim_ae = DB_READ_LEN(fctx->db, ovl->aread);
    }

    if (trim_ab >= trim_ae)
    {
        return 1;
    }

    int flips_trim_b = trim_ab;
    int flips_trim_e = trim_ae;
    if (filter_flips(fctx, ovl, novl, &flips_trim_b, &flips_trim_e))
    {
        fctx->num_flips += 1;
    }

    trim_ab = MAX(flips_trim_b, trim_ab);
    trim_ae = MIN(flips_trim_e, trim_ae);

    // sanity check tracks

    int alen = DB_READ_LEN(fctx->db, ovl->aread);
    int nsegments = (alen + fctx->twidth - 1) / fctx->twidth;

    int ob = qanno[ovl->aread] / sizeof(track_data);
    int oe = qanno[ovl->aread + 1] / sizeof(track_data);

    if (oe - ob != nsegments)
    {
        fprintf(stderr, "read %d expected %d Q track entries, found %d\n", ovl->aread, nsegments, oe - ob);
        exit(1);
    }

    ob = dustanno[ovl->aread] / sizeof(track_data);
    oe = dustanno[ovl->aread + 1] / sizeof(track_data);

    while (ob < oe)
    {
        int b = dustdata[ob];
        int e = dustdata[ob + 1];

        if (b < 0 || b > alen || b > e || e > alen)
        {
            fprintf(stderr, "dust interval %d..%d outside read length %d\n", b, e, alen);
            exit(1);
        }

        ob += 2;
    }

    if (trim_ab < 0 || trim_ab > alen || trim_ab > trim_ae || trim_ae > alen)
    {
        fprintf(stderr, "trim interval %d..%d outside read length %d\n", trim_ab, trim_ae, alen);
        exit(1);
    }

    // locate breaks in A and move outwards to the next segment boundary

    int i;
    for (i = 1; i < novl; i++)
    {
        if ( ovl[i-1].bread == ovl[i].bread &&
             ovl[i-1].path.aepos < ovl[i].path.abpos &&
             (ovl[i-1].flags & OVL_COMP) == (ovl[i].flags & OVL_COMP) )
        {
            if ( dcur >= dmax )
            {
                dmax = dmax * 1.2 + 1000;
                data = realloc(data, sizeof(Gap) * dmax);
            }

            ovl_trace* trace_left = ovl[i-1].path.trace;
            ovl_trace* trace_right = ovl[i].path.trace;

            int ab = (ovl[i-1].path.aepos - 1) / fctx->twidth;
            int ae = ovl[i].path.abpos / fctx->twidth + 1;

            int j = ovl[i-1].path.tlen - 1;

            int bb = ovl[i-1].path.bepos - trace_left[j];
            int be = ovl[i].path.bbpos + trace_right[1];

            /*
            while (qa[ab-1] > lowq)
            {
                j -= 2;
                ab--;

                bb -= trace_left[j];
            }

            j = 1;
            while (qa[ae+1] > lowq)
            {
                j += 2;
                ae++;

                be += trace_right[j];
            }
            */

            if (bb >= be)
            {
                continue;
            }

            if (ovl[i].flags & OVL_COMP)
            {
                int t = bb;
                int blen = DB_READ_LEN(fctx->db, ovl[i].bread);

                bb = blen - be;
                be = blen - t;
            }

            // check if the gap is due to a weak region in B
            track_anno ob = dustanno[ovl[i].bread] / sizeof(track_data);
            track_anno oe = dustanno[ovl[i].bread + 1] / sizeof(track_data);

            int weak_b = 0;
            while (ob < oe)
            {
                track_data db = dustdata[ob];
                track_data de = dustdata[ob+1];

                if (bb <= db && be >= de)
                {
                    weak_b = 1;
                    break;
                }

                ob += 2;
            }

            if (weak_b)
            {
                continue;
            }

            track_data* qb = qdata + (qanno[ovl[i].bread] / sizeof(track_data));
            int beg = bb / fctx->twidth;
            int end = be / fctx->twidth + 1;
            int q = 0;
            j = beg;
            while (j < end)
            {
                if (qb[j] == 0)
                {
                    weak_b = 1;
                }

                q += qb[j];
                j++;
            }

            if (weak_b)
            {
                continue;
            }

#ifdef DEBUG
            printf("A %7d %5d..%5d %5d..%5d -> ", ovl->aread, ovl[i-1].path.abpos, ovl[i-1].path.aepos, ovl[i].path.abpos, ovl[i].path.aepos);
            printf("B %7d %5d..%5d %5d..%5d | ", ovl[i].bread, ovl[i-1].path.bbpos, ovl[i-1].path.bepos, ovl[i].path.bbpos, ovl[i].path.bepos);
            printf("%d %d %d %d %d %d %d\n", ab, ae, q, beg, end, bb, be);
#endif

            // gap due to potential weak region in A

            data[dcur].bb = bb;
            data[dcur].be = be;
            data[dcur].b = ovl[i].bread;
            data[dcur].span = 0;
            data[dcur].support = 1;
            data[dcur].ab = ab * fctx->twidth;
            data[dcur].ae = ae * fctx->twidth;
            data[dcur].diff = 100.0 * q / (be - bb);
            data[dcur].comp = (ovl[i].flags & OVL_COMP);

            // printf("OVL %d..%d -> %d..%d\n", ovl[i-1].path.abpos, ovl[i-1].path.aepos, ovl[i].path.abpos, ovl[i].path.aepos);

            dcur++;
        }
    }

    qsort(data, dcur, sizeof(Gap), cmp_gaps);

    int j = 0;

    // merge breaks located at the same position in A

    for (i = 0; i < dcur; i++)
    {
        if (data[i].support == -1)
        {
            continue;
        }

        if ( maxgap != -1 && ( data[i].ae - data[i].ab >= maxgap || abs(data[i].be - data[i].bb) >= maxgap ) )
        {
            data[i].support = -1;
            continue;
        }

        for (j = i + 1; j < dcur && data[i].ab == data[j].ab && data[i].ae == data[j].ae; j++)
        {
            if (data[j].support == -1)
            {
                continue;
            }

            if ( abs( (data[j].be - data[j].bb) - (data[i].be - data[i].bb) ) < 40 )        // TODO --- hardcoded
            {
                data[i].support += 1;
                data[j].support = -1;
            }
        }
    }

    /*
    printf("############\n");

    for (i = 0; i < dcur; i++)
    {
        if ( data[i].support == - 1)
        {
            continue;
        }

        printf("%7d %7d | %5d..%5d -> %5d..%5d (%6d x %6d) @ DIFF %3d SUPPORT %3d SPAN %3d\n",
                    ovl->aread,
                    data[i].b,
                    data[i].ab, data[i].ae,
                    data[i].bb, data[i].be,
                    data[i].ae - data[i].ab, data[i].be - data[i].bb,
                    data[i].diff, data[i].support, data[i].span);

        printf("Q");
        for (j = data[i].ab / fctx->twidth; j < data[i].ae / fctx->twidth; j++)
        {
            printf(" %2d", qa[j]);
        }
        printf("\n");
    }

    printf("############\n");
    */

    // merge overlapping breaks

    for (i = 0; i < dcur; i++)
    {
        if (data[i].support == -1)
        {
            continue;
        }

        for (j = i + 1; j < dcur && data[i].ae > data[j].ab && data[i].ab < data[j].ae; j++)
        {
            if (data[j].support == -1)
            {
                continue;
            }

            if (data[i].support > data[j].support)
            {
                data[i].support += data[j].support;
                data[j].support = -1;
            }
            else
            {
                data[j].support += data[i].support;
                data[i].support = -1;
                break;
            }
        }
    }

    // filter breaks with not enough support (# B reads) or no accompanying Q drop in A

    for (j = 0; j < dcur; j++)
    {
        if ( spanners(ovl, novl, data[j].ab, data[j].ae) > 10 )
        {
            data[j].support = -1;
        }
    }

    j = 0;
    for (i = 0; i < dcur; i++)
    {
        if (data[i].support < 5)
        {
            continue;
        }

        int bad_q = 0;
        int k;
        for (k = data[i].ab / fctx->twidth; k < data[i].ae / fctx->twidth; k++)
        {
            if (qa[k] == 0 || qa[k] >= lowq)
            {
                bad_q = 1;
                break;
            }
        }

        if (!bad_q)
        {
            continue;
        }

        data[j] = data[i];
        j++;
    }

    dcur = j;

    // scan for bad regions ~1k from both ends

    int seg_first = trim_ab / fctx->twidth;
    int seg_last = trim_ae / fctx->twidth;

    while (qa[seg_first] == 0)
    {
        seg_first++;
    }

    while (qa[seg_last - 1] == 0)
    {
        seg_last--;
    }

    for (i = seg_first; i < seg_last; i++)
    {
        // TODO ---------

        // if (i == seg_first + BEND_SEGMENTS)
        // {
        //    i = MAX(i, seg_last - BEND_SEGMENTS - 1);
        // }

        if (qa[i] != 0 && qa[i] < lowq)
        {
            continue;
        }

        int ab = i * fctx->twidth;
        int ae = (i + 1) * fctx->twidth;

        // already covered by a break interval
        int contained = 0;
        for (j = 0; j < dcur; j++)
        {
            if ( data[j].ab <= ae && data[j].ae >= ab )
            {
                contained = 1;
                break;
            }
        }

        if (contained)
        {
            // printf("%d @ %d contained\n", i, qa[i]);
            continue;
        }

        /*
        // region dusted
        track_anno ob = dustanno[ovl->aread] / sizeof(track_data);
        track_anno oe = dustanno[ovl->aread + 1] / sizeof(track_data);

        int weak_a = 0;
        while (ob < oe)
        {
            track_data db = dustdata[ob];
            track_data de = dustdata[ob+1];

            if (ae >= db && ab <= de)
            {
                // printf("A dust %d..%d %d..%d\n", ab, ae, db, de);
                weak_a = 1;
                break;
            }

            ob += 2;
        }

        if (!weak_a && qa[i] < lowq)
        {
            continue;
        }
        */

        // spanners & reads starting/stopping
        int span = 0;
        int border = 0;

        float q_min = 0;
        int ovl_q_min = -1;
        int bb_q_min, be_q_min;

        for (j = 0; j < novl; j++)
        {
            // if (ovl[j].flags & OVL_COMP) continue;

            if (ovl[j].path.abpos + 100 <= ab && ovl[j].path.aepos - 100 >= ae)     // TODO --- hardcoded
            {
                // locate replacement segment(s) in B

                int bb, be;

                bb = -1;
                be = ovl[j].path.bbpos;
                ovl_trace* tracerep = ovl[j].path.trace;

                int apos = ovl[j].path.abpos;
                int k = 0;
                while (apos <= ab)
                {
                    apos = (apos / fctx->twidth + 1) * fctx->twidth;

                    bb = be;
                    be += tracerep[ k + 1 ];
                    k += 2;
                }

                assert( bb != -1 );

                if (ovl[j].flags & OVL_COMP)
                {
                    int t = bb;
                    int blen = DB_READ_LEN(fctx->db, ovl[j].bread);

                    bb = blen - be;
                    be = blen - t;
                }

                // get Q in B read
                track_data* qb = qdata + (qanno[ovl[j].bread] / sizeof(track_data));
                int beg = bb / fctx->twidth;
                int end = be / fctx->twidth;
                int q = 0;
                k = beg;
                while (k < end)
                {
                    if (qb[k] == 0)
                    {
                        q = 0;
                        break;
                    }

                    q += qb[k];
                    k++;
                }

                if (q == 0)
                {
                    continue;
                }

                // printf("%d..%d %d..%d\n", bb, be, beg, end);

                float q_new = 1.0 * q / (end - beg);

                if (ovl_q_min == -1 || q_new < q_min)
                {
                    bb_q_min = bb;
                    be_q_min = be;
                    q_min = q_new;
                    ovl_q_min = j;
                }

                // printf("%c %8d %5d..%5d %3d..%3d %5.2f\n", ovl_q_min == j ? '*' : ' ', ovl[j].bread, bb, be, beg, end, q_new);

                span++;
            }

            if ( (ovl[j].path.abpos >= ab && ovl[j].path.abpos <= ae) ||
                 (ovl[j].path.aepos >= ab && ovl[j].path.aepos <= ae) )
            {
                border++;
            }
        }

        // nothing spans the bad region or nothing starts/stops there
        if (ovl_q_min == -1) // || border == 0)
        {
            continue;
        }

        // locate replacement segment in B
        Overlap* ovlrep = ovl + ovl_q_min;

#ifdef DEBUG
        printf("in %d bad Q %d @ %d..%d SPAN %d BORDER %d\n", ovl->aread, qa[i], ab, ae, span, border);

        printf("  -> using B %d..%d x %d..%d @ %.2f\n", ovlrep->path.abpos, ovlrep->path.aepos,
                                                        ovlrep->path.bbpos, ovlrep->path.bepos,
                                                        q_min);
#endif

        data[dcur].bb = bb_q_min;
        data[dcur].be = be_q_min;
        data[dcur].b = ovlrep->bread;
        data[dcur].span = span;
        data[dcur].support = border;
        data[dcur].ab = ab;
        data[dcur].ae = ae;
        data[dcur].diff = q_min;
        data[dcur].comp = (ovlrep->flags & OVL_COMP);

        dcur++;
    }

    // no problems in read

    if (dcur == 0)
    {
        Load_Read(fctx->db, ovl->aread, fctx->reada, 1);

        if (trim_ae - trim_ab >= fctx->minlen)
        {
            fprintf(fctx->fileFastaOut, ">trimmed_%d source=%d",
                            ovl->aread,
                            ovl->aread);

            for (i = 0; i < fctx->curctracks; i++)
            {
                track_anno* anno = fctx->convertTracks[i]->anno;
                track_data* data = fctx->convertTracks[i]->data;
                track_anno ob = anno[ ovl->aread ] / sizeof(track_data);
                track_anno oe = anno[ ovl->aread + 1 ] / sizeof(track_data);
                char* track = fctx->convertTracks[i]->name;

                int first = 1;
                int beg, end;
                for( ; ob < oe ; ob += 2)
                {
                    beg = data[ob] - trim_ab;
                    end = data[ob + 1] - trim_ab;

                    // check trim begin
                    if( end < 0 )
                    {
                        continue;
                    }

                    if( beg < 0 )
                    {
                        beg = 0;
                    }

                    // check trim end
                    if ( beg > trim_ae - trim_ab )
                    {
                        break;
                    }

                    if ( end > trim_ae - trim_ab )
                    {
                        end = (trim_ae - trim_ab);
                    }

                    if (first)
                    {
                        fprintf(fctx->fileFastaOut, " %s=", track);
                    }
                    else
                    {
                        fprintf(fctx->fileFastaOut, ",");
                    }

                    fprintf(fctx->fileFastaOut, "%d,%d", beg, end);

                    first = 0;
                }

            }

            fprintf(fctx->fileFastaOut, "\n");

            wrap_write(fctx->fileFastaOut, fctx->reada + trim_ab, trim_ae - trim_ab, FASTA_WIDTH);

            if (fctx->fileQvOut)
            {
                Load_QVentry(fctx->db, ovl->aread, fctx->qva, 1);

                fprintf(fctx->fileQvOut, "@fixed/%d_%d source=%d\n", 0, trim_ae - trim_ab, ovl->aread);

                for (i = 0 ; i < NUM_QV_STREAMS ; i++)
                {
                    fprintf(fctx->fileQvOut, "%.*s\n", trim_ae - trim_ab, fctx->qva[i] + trim_ab);
                }
            }
        }

        // cleanup
        free(data);

        return 1;
    }

    qsort(data, dcur, sizeof(Gap), cmp_gaps);

    // count reads that span the break

    for (i = 0; i < novl; i++)
    {
        for (j = 0; j < dcur; j++)
        {
            if (ovl[i].path.abpos + 100 < data[j].ab && ovl[i].path.aepos - 100 > data[j].ae)       // TODO --- hardcoded
            {
                data[j].span += 1;
            }
        }
    }

    // calculate new read length and patch segments

    Load_Read(fctx->db, ovl->aread, fctx->reada, 1);

    if (fctx->fileQvOut)
    {
        Load_QVentry(fctx->db, ovl->aread, fctx->qva, 1);
    }

    char* read = fctx->read_patched;
    char** qv = fctx->qv_patched;
    int rlen = 0;

    int ab = trim_ab;
    int ae;

#ifdef DEBUG
    printf("A %7d TRIM %5d..%5d\n", ovl->aread, trim_ab, trim_ae);
#endif

    int* apatches = fctx->apatches;
    int napatches = 0;

    for (i = 0; i < dcur; i++)
    {
        if ( trim_ab > data[i].ab )
        {
            ab = data[i].ae;
            continue;
        }

        if ( trim_ae < data[i].ae )
        {
            // ae = data[i].ae;
            break;
        }

        ae = data[i].ab;

        if (trim_ab < ae && trim_ab > ab)
        {
            ab = trim_ab;
        }

        // A[ab..ae]

        assert(ab <= ae);

        if (ab < ae)
        {
#ifdef DEBUG
            printf("A %7d %5d..%5d\n", ovl->aread, ab, ae);
#endif
            apatches[napatches] = ab;
            apatches[napatches + 1] = ae;
            apatches[napatches + 2] = rlen;
            napatches += 3;

            if (fctx->fileQvOut)
            {
                for (j = 0; j < NUM_QV_STREAMS; j++)
                {
                    memcpy( qv[j] + rlen, fctx->qva[j] + ab, ae - ab );
                }
            }

            memcpy(read + rlen, fctx->reada + ab, ae - ab);
            rlen += ae - ab;
        }

        ab = data[i].ae;

        // B[bb..be]

        fctx->num_gaps += 1;

        int bb = data[i].bb;
        int be = data[i].be;

        fctx->stats_bases_before += data[i].ae - data[i].ab;
        fctx->stats_bases_after  += data[i].be - data[i].bb;

        if (fctx->fileQvOut)
        {
            Load_QVentry(fctx->db, data[i].b, fctx->qvb, 1);

            for (j = 0; j < NUM_QV_STREAMS; j++)
            {
                if (data[i].comp)
                {
                    rev(fctx->qvb[j] + bb, be - bb);
                }

                memcpy( qv[j] + rlen, fctx->qvb[j] + bb, be - bb );
            }
        }

        Load_Read(fctx->db, data[i].b, fctx->readb, 1);

        if (data[i].comp)
        {
            revcomp(fctx->readb + bb, be - bb);
        }

        memcpy(read + rlen, fctx->readb + bb, be - bb);
        rlen += be - bb;

#ifdef DEBUG
        printf("B %7d %5d..%5d (%6d) @ DIFF %3d SUPPORT %3d SPAN %3d",
                    data[i].b, bb, be,
                    be - bb,
                    data[i].diff, data[i].support, data[i].span);

        printf("    Q");
        for (j = data[i].ab / fctx->twidth; j < data[i].ae / fctx->twidth; j++)
        {
            printf(" %2d", qa[j]);
        }
        printf("\n");
#endif

    }

    ae = trim_ae;

    if (ab < ae)
    {
        apatches[napatches] = ab;
        apatches[napatches + 1] = ae;
        apatches[napatches + 2] = rlen;
        napatches += 3;

        if (fctx->fileQvOut)
        {
            for (j = 0; j < NUM_QV_STREAMS; j++)
            {
                memcpy( qv[j] + rlen, fctx->qva[j] + ab, ae - ab );
            }
        }

        memcpy(read + rlen, fctx->reada + ab, ae - ab);
        rlen += ae - ab;

#ifdef DEBUG
        printf("A %7d %5d..%5d\n", ovl->aread, ab, ae);
#endif
    }

#ifdef DEBUG
    printf("A %7d RLEN %5d -> %5d\n", ovl->aread, DB_READ_LEN(fctx->db, ovl->aread), rlen);
#endif

    // write patched sequence

    if (rlen >= fctx->minlen)
    {
        fprintf(fctx->fileFastaOut, ">fixed_%d source=%d",
                        ovl->aread,
                        ovl->aread);

#ifdef DEBUG_INTERVAL_ADJUSTMENT
        printf("\n\n");
        for (i = 0; i < napatches; i += 3)
        {
            printf("A-PATCH %5d..%5d -> %5d\n", apatches[i], apatches[i+1], apatches[i+2]);
        }
#endif

        // for each track
        for (i = 0; i < fctx->curctracks; i++)
        {
            track_anno* anno = fctx->convertTracks[i]->anno;
            track_data* data = fctx->convertTracks[i]->data;
            track_anno ob = anno[ ovl->aread ] / sizeof(track_data);
            track_anno oe = anno[ ovl->aread + 1 ] / sizeof(track_data);
            char* track = fctx->convertTracks[i]->name;

            // adjust intervals if present
            if (ob < oe)
            {
                int first = 1;

                while (ob < oe)
                {
                    int ib = data[ob];
                    int ie = data[ob + 1];
                    int ib_adj = -1;
                    int ie_adj = -1;

                    if ( ie < apatches[0] || ib > apatches[napatches - 2] )
                    {
#ifdef DEBUG_INTERVAL_ADJUSTMENT
                        printf("INTRVL  %5d..%5d -> OUTSIDE\n", ib, ie);
#endif

                        ob += 2;
                        continue;
                    }

                    for (j = 0; j < napatches; j += 3)
                    {
                        if (ib_adj == -1)
                        {
                            if (ib < apatches[j+1])
                            {
                                ib_adj = MAX(ib, apatches[j]);
                                ib_adj = apatches[j+2] + (ib_adj - apatches[j]);
                            }
                        }

                        if (ie_adj == -1)
                        {
                            if (ie <= apatches[j+1])
                            {
                                if (ie < apatches[j] && j > 0)
                                {
                                    ie_adj = apatches[j - 2];
                                    ie_adj = apatches[j - 1] + (ie_adj - apatches[j - 3]);

                                    break ;
                                }
                                else if (ie > apatches[j])
                                {
                                    ie_adj = ie;
                                    ie_adj = apatches[j + 2] + (ie_adj - apatches[j]);

                                    break ;
                                }
                            }
                        }
                    }

                    if (ie_adj - ib_adj > MIN_INT_LEN)
                    {
#ifdef DEBUG_INTERVAL_ADJUSTMENT
                        printf("INTRVL  %5d..%5d -> %5d..%5d\n", ib, ie, ib_adj, ie_adj);
#endif

                        if (!first)
                        {
                            fprintf(fctx->fileFastaOut, ",");
                        }
                        else
                        {
                            fprintf(fctx->fileFastaOut, " %s=", track);
                        }

                        // sanity check
                        if (ib_adj < 0 || ib_adj > rlen || ib_adj > ie_adj || ie_adj > rlen)
                        {
                            fprintf(stderr, "adjust interval %d..%d outside read length %d\n", ib_adj, ie_adj, rlen);
                            exit(1);
                        }

                        fprintf(fctx->fileFastaOut, "%d,%d", ib_adj, ie_adj);

                        first = 0;
                    }
                    else
                    {
#ifdef DEBUG_INTERVAL_ADJUSTMENT
                        printf("INTRVL  %5d..%5d -> SKIP\n", ib, ie);
#endif
                    }

                    ob += 2;
                }
            }
        }

        fprintf(fctx->fileFastaOut, "\n");

        wrap_write(fctx->fileFastaOut, read, rlen, FASTA_WIDTH);

        if (fctx->fileQvOut)
        {
            fprintf(fctx->fileQvOut, "@fixed/%d_%d source=%d\n", 0, rlen, ovl->aread);

            for (j = 0; j < NUM_QV_STREAMS; j++)
            {
                fprintf(fctx->fileQvOut, "%.*s\n", rlen, qv[j]);
            }
        }
    }

    // cleanup
    free(data);

    return 1;
}

static void usage()
{
    printf("usage: [-xQg <int>] [-ct <track>] [-q <patched.quiva>] <db> <in.las> <patched.fasta>\n");
    printf("       -c ... convert track intervals (multiple -c possible)\n");
    printf("       -q ... patch quality streams\n");
    printf("       -x ... min length for fixed sequences (%d)\n", DEF_ARG_X);
    printf("       -Q ... segment quality threshold (%d)\n", DEF_ARG_Q);
    printf("       -g ... max gap length for patching (%d)\n", DEF_ARG_G);
    printf("       -t ... trim reads based on a track\n");
}

int main(int argc, char* argv[])
{
    HITS_DB db;
    PassContext* pctx;
    FixContext fctx;
    FILE* fileOvlIn;

    bzero(&fctx, sizeof(FixContext));
    fctx.db = &db;
    fctx.minlen = DEF_ARG_X;
    fctx.lowq = DEF_ARG_Q;
    fctx.maxgap = DEF_ARG_G;
    fctx.trimName = NULL;

    // process arguments

    char* pathQvOut = NULL;
    int c;
    opterr = 0;

    while ((c = getopt(argc, argv, "x:q:c:Q:g:t:")) != -1)
    {
        switch (c)
        {
            case 'Q':
                      fctx.lowq = atoi(optarg);
                      break;

            case 'g':
                      fctx.maxgap = atoi(optarg);
                      break;

            case 'x':
                      fctx.minlen = atoi(optarg);
                      break;

            case 'q':
                      pathQvOut = optarg;
                      break;

            case 't':
                      fctx.trimName = optarg;
                      break;

            case 'c':
                      if (fctx.curctracks >= fctx.maxctracks)
                      {
                          fctx.maxctracks += 10;
                          fctx.convertTracks = realloc(fctx.convertTracks, sizeof(HITS_TRACK*) * fctx.maxctracks);
                      }

                      // use the HITS_TRACK* array as temporary storage of the track names

                      fctx.convertTracks[ fctx.curctracks ] = (HITS_TRACK*)optarg;
                      fctx.curctracks++;

                      break;

            default:
                      usage();
                      exit(1);
        }
    }

    if (opterr || argc - optind != 3)
    {
        usage();
        exit(1);
    }

    char* pcPathReadsIn = argv[optind++];
    char* pcPathOverlapsIn = argv[optind++];
    char* pcPathFastaOut = argv[optind++];

    if ( (fileOvlIn = fopen(pcPathOverlapsIn, "r")) == NULL )
    {
        fprintf(stderr, "could not open '%s'\n", pcPathOverlapsIn);
        exit(1);
    }

    if ( (fctx.fileFastaOut = fopen(pcPathFastaOut, "w")) == NULL )
    {
        fprintf(stderr, "could not open '%s'\n", pcPathFastaOut);
        exit(1);
    }

    if (pathQvOut)
    {
        if ( (fctx.fileQvOut = fopen(pathQvOut, "w")) == NULL )
        {
            fprintf(stderr, "error: could not open '%s'\n", pathQvOut);
            exit(1);
        }
    }

    if ( Open_DB(pcPathReadsIn, &db) )
    {
        fprintf(stderr, "could not open database '%s'\n", pcPathReadsIn);
        exit(1);
    }

    int i;
    for (i = 0; i < fctx.curctracks; i++)
    {
        char* track = (char*)fctx.convertTracks[i];
        fctx.convertTracks[i] = track_load(&db, track);

        if (fctx.convertTracks[i] == NULL)
        {
            fprintf(stderr, "could not open track '%s'\n", track);
            exit(1);
        }
    }

    // pass

    if (fctx.fileQvOut)
    {
        if (Load_QVs(&db) != 0)
        {
            fprintf(stderr, "error: failed to load QVs\n");
            exit(1);
        }
    }

    pctx = pass_init(fileOvlIn, NULL);

    pctx->split_b = 0;
    pctx->load_trace = 1;
    pctx->unpack_trace = 1;
    pctx->data = &fctx;

    fix_pre(pctx, &fctx);

    pass(pctx, fix_process);

    fix_post(pctx, &fctx);

    pass_free(pctx);

    // cleanup

    if (fctx.fileQvOut)
    {
        Close_QVs(&db);
    }

    Close_DB(&db);

    fclose(fileOvlIn);
    fclose(fctx.fileFastaOut);

    return 0;
}