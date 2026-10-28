/* Fixed upstream libfvad source amalgamation; files remain unmodified.
 * See upstream.json, LICENSE, PATENTS and AUTHORS. */
#ifndef S12_FIXED_LIBFVAD_IMPLEMENTATION
#define S12_FIXED_LIBFVAD_IMPLEMENTATION
/* Upstream fvad_reset keeps an assert-only status local under NDEBUG. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "src/fvad.c"
#include "src/signal_processing/division_operations.c"
#include "src/signal_processing/energy.c"
#include "src/signal_processing/get_scaling_square.c"
#include "src/signal_processing/resample_48khz.c"
#include "src/signal_processing/resample_by_2_internal.c"
#include "src/signal_processing/resample_fractional.c"
#include "src/signal_processing/spl_inl.c"
#include "src/vad/vad_core.c"
#include "src/vad/vad_filterbank.c"
#include "src/vad/vad_gmm.c"
#include "src/vad/vad_sp.c"
#pragma GCC diagnostic pop
#endif
