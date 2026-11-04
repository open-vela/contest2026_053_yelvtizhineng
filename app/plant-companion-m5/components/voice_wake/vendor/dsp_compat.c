/* ESP-DSP v1.4.0 reference kernels, ABI wrappers for ESP-SR v2.0.0.
 * No placeholder FFTs: every wrapper calls the corresponding real kernel.
 */
#include <stdbool.h>
#include "dsps_fft2r.h"
#include "dsps_fft4r.h"
bool dsp_is_power_of_two(int x){return x>0 && !(x&(x-1));}
int dsp_power_of_two(int x){int n=0;while(x>1){x>>=1;n++;}return n;}
int dsps_fft2r_fc32_aes3_(float *p,int n,float *w){return dsps_fft2r_fc32_ansi_(p,n,w);}
int dsps_fft4r_fc32_ae32_(float *p,int n,float *w,int size){return dsps_fft4r_fc32_ansi_(p,n,w,size);}
int dsps_cplx2real_fc32_ae32_(float *p,int n,float *w,int size){return dsps_cplx2real_fc32_ansi_(p,n,w,size);}
int dsps_bit_rev4r_fc32_ae32(float *p,int n){return dsps_bit_rev4r_fc32(p,n);}
int dsps_dotprod_f32_aes3(const float *a,const float *b,float *out,int n)
{float sum=0;for(int i=0;i<n;i++)sum+=a[i]*b[i];*out=sum;return 0;}
