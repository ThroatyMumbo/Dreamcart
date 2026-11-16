// Audio lists as AICA voices (alist_aica.c), the any-cart ELFs' default
#include "hle_internal.h"

#include "alist_aica.h"
#include "rsp_audio.h"

void audio_run(struct hle_t *h, int abi, uint32_t hz) { alist_aica_run(h, abi, hz); }
