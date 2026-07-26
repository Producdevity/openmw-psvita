/*
 * Keep ffmpeg's real av_tx (Bink cinematic audio needs the float
 * RDFT/DCT codelets) while excluding the unused double/int32 codelet
 * objects and their ~12.6MB of BSS tables. Defining their codelet
 * lists here (empty) satisfies tx.o's references so tx_double.o and
 * tx_int32.o are never pulled from libavutil.a; tx_float.o (~4MB BSS)
 * links normally.
 *
 * History: this file used to stub av_tx_init itself to always fail —
 * that silently broke every av_tx-based decoder (binkaudio = silent
 * cinematics; aac/vorbis would have been silent too).
 */
#ifdef __vita__

typedef struct FFTXCodelet FFTXCodelet;

const FFTXCodelet* const ff_tx_codelet_list_double_c[] = { 0 };
const FFTXCodelet* const ff_tx_codelet_list_int32_c[] = { 0 };

#endif /* __vita__ */
