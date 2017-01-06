/*
    PiFmRds - FM/RDS transmitter for the Raspberry Pi
    Copyright (C) 2014 Christophe Jacquet, F8FTK
    
    See https://github.com/ChristopheJacquet/PiFmRds
    
    rds_wav.c is a test program that writes a RDS baseband signal to a WAV
    file. It requires libsndfile.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
    
    fm_mpx.c: generates an FM multiplex signal containing RDS plus possibly
    monaural or stereo audio.
*/

#include <sndfile.h>
#include <stdlib.h>
#include <strings.h>
#include <math.h>

#include "rds.h"


#define PI 3.141592654


#define FIR_PHASES    (32)
#define FIR_SIZE    (1024)   // MUST be a power of 2 for the circular buffer
#define FIR_TAPS    (FIR_SIZE/FIR_PHASES)

size_t length;

// coefficients of the low-pass FIR filter
float low_pass_fir[FIR_SIZE];


float carrier_38[] = {0.0, 0.8660254037844386, 0.8660254037844388, 1.2246467991473532e-16, -0.8660254037844384, -0.8660254037844386};

float carrier_19[] = {0.0, 0.5, 0.8660254037844386, 1.0, 0.8660254037844388, 0.5, 1.2246467991473532e-16, -0.5, -0.8660254037844384, -1.0, -0.8660254037844386, -0.5};
    
int phase_38 = 0;
int phase_19 = 0;


float downsample_factor;


float *audio_buffer;
int audio_index = 0;
int audio_len = 0;
float audio_pos;

float fir_buffer_left[FIR_SIZE] = {0};
float fir_buffer_right[FIR_SIZE] = {0};
int fir_index = 0;
int channels;

SNDFILE *inf;



float *alloc_empty_buffer(size_t length) {
    float *p = malloc(length * sizeof(float));
    if(p == NULL) return NULL;
    
    bzero(p, length * sizeof(float));
    
    return p;
}


int fm_mpx_open(char *filename, size_t len) {
    length = len;

    if(filename != NULL) {
        // Open the input file
        SF_INFO sfinfo;
 
        // stdin or file on the filesystem?
        if(filename[0] == '-') {
            if(! (inf = sf_open_fd(fileno(stdin), SFM_READ, &sfinfo, 0))) {
                fprintf(stderr, "Error: could not open stdin for audio input.\n") ;
                return -1;
            } else {
                printf("Using stdin for audio input.\n");
            }
        } else {
            if(! (inf = sf_open(filename, SFM_READ, &sfinfo))) {
                fprintf(stderr, "Error: could not open input file %s.\n", filename) ;
                return -1;
            } else {
                printf("Using audio file: %s\n", filename);
            }
        }
            
        int in_samplerate = sfinfo.samplerate;
        downsample_factor = 228000. / in_samplerate;
    
        printf("Input: %d Hz, upsampling factor: %.2f\n", in_samplerate, downsample_factor);

        channels = sfinfo.channels;
        if(channels > 1) {
            printf("%d channels, generating stereo multiplex.\n", channels);
        } else {
            printf("1 channel, monophonic operation.\n");
        }
    
        // Choose a cutoff frequency for the low-pass FIR filter
        float cutoff_freq = 15700;
        if(in_samplerate/2 < cutoff_freq) cutoff_freq = in_samplerate/2 * .8;
   

        // Create the low-pass FIR filter, with pre-emphasis
        double window, firlowpass, firpreemph , sincpos;
        double gain=FIR_PHASES/25.0; // Why??? Maybe gain adjustment for preemphais

        // IIR pre-emphasis filter
        // Reference material:    http://jontio.zapto.org/hda1/preempiir.pdf
        double tau=75e-6;
        double delta=1.96e-6;
        double taup, deltap, bp, ap, a0, a1, b1;
        taup=1.0/(2.0*(in_samplerate*FIR_PHASES))/tan(  1.0/(2*tau*(in_samplerate*FIR_PHASES) ));
        deltap=1.0/(2.0*(in_samplerate*FIR_PHASES))/tan(  1.0/(2*delta*(in_samplerate*FIR_PHASES) ));
        bp=sqrt( -taup*taup + sqrt(taup*taup*taup*taup + 8.0*taup*taup*deltap*deltap) ) / 2.0 ;
        ap=sqrt( 2*bp*bp + taup*taup );
        a0=( 2.0*ap + 1/(in_samplerate*FIR_PHASES) )/(2.0*bp + 1/(in_samplerate*FIR_PHASES) );
        a1=(-2.0*ap + 1/(in_samplerate*FIR_PHASES) )/(2.0*bp + 1/(in_samplerate*FIR_PHASES) );
        b1=( 2.0*bp + 1/(in_samplerate*FIR_PHASES) )/(2.0*bp + 1/(in_samplerate*FIR_PHASES) );
        double x=0,y=0;
 
        for(int i=1; i<=FIR_SIZE; i++) {   // match indexing of Matlab script
            sincpos = i-((FIR_SIZE+1.0)/2.0); // offset by 0.5 so sincpos!=0 (causes NaN x/0 )
            //printf("%d=%f ", i,sincpos); 
            firlowpass = sin(2 * PI * cutoff_freq * sincpos / (in_samplerate*FIR_PHASES) ) / (PI * sincpos) ; 

            y=a0*firlowpass + a1*x + b1*y ; // Find the combined impulse response
            x=firlowpass;                   // of FIR low-pass and IIR pre-emphasis
            firpreemph=y;                   // y could be replaced by firpreemph but this
                                            // matches the example in the reference material

            window = (.54 - .46 * cos(2*PI * (i) / (double) FIR_SIZE )) ; // Hamming window
            low_pass_fir[i-1] = firpreemph * window * gain ; // store with C indexing
        } 
    
        printf("Created low-pass FIR filter for audio channels, with cutoff at %.1f Hz\n", cutoff_freq);
    
        if( 0 )
        {
          printf("f = [ ");
          for(int i=0; i<FIR_SIZE; i++)
          {
            printf("%.5f ", low_pass_fir[i]);
            //printf("%i %.5f \n", i,low_pass_fir[i]);
          }
          printf("]; \n");
        }
        
        audio_pos = downsample_factor;
        audio_buffer = alloc_empty_buffer(length * channels);
        if(audio_buffer == NULL) return -1;

    } // end if(filename != NULL)
    else {
        inf = NULL;
        // inf == NULL indicates that there is no audio
    }
    
    return 0;
}


// samples provided by this function are in 0..10: they need to be divided by
// 10 after.
int fm_mpx_get_samples(float *mpx_buffer) {
    get_rds_samples(mpx_buffer, length);

    if(inf  == NULL) return 0; // if there is no audio, stop here
    
    for(int i=0; i<length; i++) {
        if(audio_pos >= downsample_factor) {
            audio_pos -= downsample_factor;
            
            if(audio_len <= channels ) {
                for(int j=0; j<2; j++) { // one retry
                    audio_len = sf_read_float(inf, audio_buffer, length);
                    if (audio_len < 0) {
                        fprintf(stderr, "Error reading audio\n");
                        return -1;
                    }
                    if(audio_len == 0) {
                        if( sf_seek(inf, 0, SEEK_SET) < 0 ) {
                            fprintf(stderr, "Could not rewind in audio file, terminating\n");
                            return -1;
                        }
                    } else {
                        break;
                    }
                }
                audio_index = 0;
            } else {
                audio_index += channels;
                audio_len -= channels;
            }

           fir_index++;  // fir_index will point to newest valid data soon
           if(fir_index >= FIR_SIZE) fir_index = 0; 
           // Store the current sample(s) into the FIR filter's ring buffer
           fir_buffer_left[fir_index] = audio_buffer[audio_index];
           if(channels > 1) { 
               fir_buffer_right[fir_index] =  audio_buffer[audio_index+1]; 
           }
        } // if need new sample

        // Polyphase FIR filter
        float out_left  = 0;
        float out_right = 0;
        // Calculate which FIR phase to use
        //int iphase = FIR_PHASES-1 - ((int) (audio_pos/downsample_factor*FIR_PHASES) );
        int iphase =  ((int) (audio_pos*FIR_PHASES/downsample_factor) );// I think this is correct
        //int iphase=FIR_PHASES-1;  // test override
        //printf("%d %d \n",fir_index,iphase); // diagnostics
	// Sanity checks
        if ( iphase < 0 ) {iphase=0; printf("low\n"); }
        if ( iphase >= FIR_PHASES ) {iphase=FIR_PHASES-2; printf("high\n"); }
        int fir_start = (fir_index - FIR_TAPS);
        if( fir_start < 0 )  fir_start+=FIR_SIZE;
        if( channels > 1 )
        {
          for(int fi=0; fi<FIR_TAPS; fi++)  // fi = Filter Index
          {                                 // use bit masking to implement circular buffer
            out_left +=low_pass_fir[ iphase + (FIR_PHASES*fi) ]*fir_buffer_left[(fir_index-fi)&(FIR_SIZE-1)];
            out_right+=low_pass_fir[ iphase + (FIR_PHASES*fi) ]*fir_buffer_right[(fir_index-fi)&(FIR_SIZE-1)];
          }
        }
        else 
        {
          for(int fi=0; fi<FIR_TAPS; fi++)  // fi = Filter Index
          {                                 // use bit masking to implement circular buffer
            out_left+=low_pass_fir[ iphase + (FIR_PHASES*fi) ] * fir_buffer_left[(fir_index-fi)&(FIR_SIZE-1)];
          }
        }
        
        // Generate the stereo mpx
        if( channels > 1 ) {
            mpx_buffer[i] +=  4.05*(out_left+out_right) + // Stereo sum signal
                4.05 * carrier_38[phase_38] * (out_left-out_right) + // Stereo difference signal
                .9*carrier_19[phase_19];                  // Stereo pilot tone

            phase_19++;
            phase_38++;
            if(phase_19 >= 12) phase_19 = 0;
            if(phase_38 >= 6) phase_38 = 0;
        }
        else
        {
            mpx_buffer[i] =  
                mpx_buffer[i] +    // RDS data samples are currently in mpx_buffer
                9.0*out_left;      // Unmodulated monophonic signal
        } 
            
        audio_pos++;   
        
    }
    
    return 0;
}


int fm_mpx_close() {
    if(sf_close(inf) ) {
        fprintf(stderr, "Error closing audio file");
    }
    
    if(audio_buffer != NULL) free(audio_buffer);
    
    return 0;
}
