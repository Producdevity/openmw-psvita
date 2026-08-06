/*
 * Keep real av_tx float codelets (Bink audio needs them);
 * empty double/int32 codelet lists drop ~12.6MB unused tables.
 * Old always-fail stub silently broke av_tx decoders.
 */
#ifdef __vita__

typedef struct FFTXCodelet FFTXCodelet;

const FFTXCodelet* const ff_tx_codelet_list_double_c[] = { 0 };
const FFTXCodelet* const ff_tx_codelet_list_int32_c[] = { 0 };

#endif /* __vita__ */
