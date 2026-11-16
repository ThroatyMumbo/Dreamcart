// The per-second report's AICA lines
#include <kos.h>
#include "play.h"
#include "rcp.h"
#include "aica_cache.h"
#include "aica_voice.h"
#include "alist_aica.h"

// The AICA path's window: voices playing now, then counts since the last report
void stats_report_audio(void) {
    static struct av_stats v0;
    static struct aa_stats a0;
    static uint32_t dry0, frames0;
    unsigned long second = stats_second;
    printf("aica %2lu: ai %lu of %lu Hz, dry %lu | voices %2lu keyons %lu underruns %lu overruns %lu dropped %lu "
           "resumed %lu cut %lu KB %lu | lists %lu cmds %lu run %lu unattributed %lu mixer-fed %lu kept resamples %lu\n", second,
           (unsigned long)(rcp_ai_frames - frames0), (unsigned long)rcp_ai_hz(), (unsigned long)(rcp_ai_dry - dry0),
           (unsigned long)av_stats.active,
           (unsigned long)(av_stats.keyons - v0.keyons), (unsigned long)(av_stats.underruns - v0.underruns),
           (unsigned long)(av_stats.overruns - v0.overruns), (unsigned long)(av_stats.dropped - v0.dropped),
           (unsigned long)(av_stats.resumed - v0.resumed), (unsigned long)(av_stats.cut - v0.cut),
           (unsigned long)((av_stats.words - v0.words) >> 8), (unsigned long)(aa_stats.lists - a0.lists),
           (unsigned long)(aa_stats.cmds - a0.cmds), (unsigned long)(aa_stats.executed - a0.executed),
           (unsigned long)(aa_stats.unattributed - a0.unattributed), (unsigned long)(aa_stats.mixed - a0.mixed),
           (unsigned long)(aa_stats.kept_resamples - a0.kept_resamples));
    if (AV_CACHE) {
        printf("cache %2lu: resident keyons %lu samples %lu decoded frames %lu | patches %lu lost %lu (foreign %lu "
               "unmapped %lu outran %lu) late %lu | cold foreign %lu unkeyed %lu refused %lu | finals %lu parts %lu KB %lu "
               "evicted %lu reopened %lu\n", second,
               (unsigned long)(av_stats.res_keyons - v0.res_keyons), (unsigned long)(av_stats.res_samples - v0.res_samples),
               (unsigned long)(av_stats.decoded_frames - v0.decoded_frames), (unsigned long)(av_stats.patches - v0.patches),
               (unsigned long)(av_stats.lost - v0.lost), (unsigned long)(av_stats.why_foreign - v0.why_foreign),
               (unsigned long)(av_stats.why_unmapped - v0.why_unmapped), (unsigned long)(av_stats.outran - v0.outran),
               (unsigned long)(av_stats.late - v0.late), (unsigned long)(av_stats.cold_foreign - v0.cold_foreign),
               (unsigned long)(av_stats.cold_unkeyed - v0.cold_unkeyed), (unsigned long)(av_stats.cold_refused - v0.cold_refused),
               (unsigned long)ac_stats.finals, (unsigned long)ac_stats.parts,
               (unsigned long)(ac_stats.bytes >> 10), (unsigned long)ac_stats.evicted, (unsigned long)ac_stats.reopened);
        for (uint32_t i = 0; i < ac_nfinals; i++)
            printf("final %08lx rom %u how %u len %lu lsa %lu lea %lu frames %lu\n", (unsigned long)ac_finals[i].at,
                   ac_finals[i].rom, ac_finals[i].how, (unsigned long)ac_finals[i].len, (unsigned long)ac_finals[i].lsa,
                   (unsigned long)ac_finals[i].lea, (unsigned long)ac_finals[i].frames);
        ac_nfinals = 0;
    }
    if (AV_CACHE)
        printf("streams %2lu: followed %lu, foreign: segments %lu adpcms %lu move depth %lu load %lu other %lu unwritten %lu "
               "clear only %lu | streamed updates: handed off %lu no entry %lu (silent %lu) | dropped at key-on %lu | why gap "
               "%lu unmapped %lu head %lu book %lu stage %lu | ADPCM key-ons %lu\n", second,
               (unsigned long)(aa_stats.foreign[0] - a0.foreign[0]), (unsigned long)(aa_stats.foreign[AA_FG_SEGS] - a0.foreign[AA_FG_SEGS]),
               (unsigned long)(aa_stats.foreign[AA_FG_ADPCMS] - a0.foreign[AA_FG_ADPCMS]),
               (unsigned long)(aa_stats.foreign[AA_FG_DEPTH] - a0.foreign[AA_FG_DEPTH]),
               (unsigned long)(aa_stats.foreign[AA_FG_LOAD] - a0.foreign[AA_FG_LOAD]),
               (unsigned long)(aa_stats.foreign[AA_FG_OTHER] - a0.foreign[AA_FG_OTHER]),
               (unsigned long)(aa_stats.foreign[AA_FG_NONE] - a0.foreign[AA_FG_NONE]),
               (unsigned long)(aa_stats.foreign[AA_FG_CLEAR] - a0.foreign[AA_FG_CLEAR]),
               (unsigned long)(av_stats.streamed_gone - v0.streamed_gone), (unsigned long)(av_stats.streamed_cold - v0.streamed_cold),
               (unsigned long)(av_stats.streamed_silent - v0.streamed_silent),
               (unsigned long)(av_stats.cold_dropped - v0.cold_dropped),
               (unsigned long)(av_stats.why_gap - v0.why_gap), (unsigned long)(av_stats.why_unmapped - v0.why_unmapped),
               (unsigned long)(av_stats.why_head - v0.why_head), (unsigned long)(av_stats.why_book - v0.why_book),
               (unsigned long)(av_stats.why_stage - v0.why_stage), (unsigned long)(aa_stats.adpcm_keyons - a0.adpcm_keyons));
    v0 = av_stats;
    a0 = aa_stats;
    dry0 = rcp_ai_dry;
    frames0 = rcp_ai_frames;
}
