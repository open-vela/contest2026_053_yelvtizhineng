/* Test-only one-shot. Never turns a failed frame into success. */
#define BODY_TIMEOUT_DIAGNOSTIC 1
#define BODY_TIMEOUT_DIAG_BUDGET_MS 180u
static bool g_timeout_diag_active,g_timeout_diag_seen,g_timeout_diag_printed;
static uint32_t g_timeout_diag_start;
struct body_timeout_sample {uint32_t at;int rc,pos,load,current,healthy;};
static struct body_timeout_sample g_timeout_samples[2];
static unsigned g_timeout_sample_count,g_timeout_diag_id,g_timeout_diag_elapsed;
static void body_timeout_diagnose(unsigned id);
static void body_timeout_diagnostic_print(void);
