/*
Copyright (C) 1994-1995 Apogee Software, Ltd.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
/**********************************************************************
   module: MULTIVOC.C

   author: James R. Dose
   date:   December 20, 1993

   Routines to provide multichannel digitized sound playback for
   Sound Blaster compatible sound cards.

   (c) Copyright 1993 James R. Dose.  All Rights Reserved.
**********************************************************************/

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include "linklist.h"
#include "sndcards.h"
#include "drivers.h"
#include "pitch.h"
#include "multivoc.h"
#include "assmisc.h"
#include "_multivc.h"

#ifdef _XBOX_APU
#include <hal/apu.h>
static int MV_ApuInitialized = 0;
int MV_ApuActive = 0;  // Global: 1 if APU hardware mixing is active
#endif

#define RoundFixed( fixedval, bits )            \
        (                                       \
          (                                     \
            (fixedval) + ( 1 << ( (bits) - 1 ) )\
          ) >> (bits)                           \
        )

#define IS_QUIET( ptr )  ( ( void * )( ptr ) == ( void * )&MV_VolumeTable[ 0 ] )

static int       MV_ReverbLevel;
static int       MV_ReverbDelay;
static VOLUME16 *MV_ReverbTable = NULL;

//static signed short MV_VolumeTable[ MV_MaxVolume + 1 ][ 256 ];
static signed short MV_VolumeTable[ 63 + 1 ][ 256 ];

//static Pan MV_PanTable[ MV_NumPanPositions ][ MV_MaxVolume + 1 ];
Pan MV_PanTable[ MV_NumPanPositions ][ 63 + 1 ];

int MV_Installed   = FALSE;
static int MV_TotalVolume = MV_MaxTotalVolume;
static int MV_MaxVoices   = 1;
static int MV_Recording;

static unsigned int MV_BufferSize = MixBufferSize;
static unsigned int MV_BufferLength;

static int MV_NumberOfBuffers = NumberOfBuffers;

static int MV_MixMode    = MONO_8BIT;
static int MV_Channels   = 1;
static int MV_Bits       = 8;

static int MV_Silence    = SILENCE_8BIT;
static int MV_SwapLeftRight = FALSE;

static int MV_RequestedMixRate;
int MV_MixRate;

static int MV_BuffShift;

static int MV_TotalMemory;

static int   MV_BufferEmpty[ NumberOfBuffers ];
char *MV_MixBuffer[ NumberOfBuffers + 1 ];

#ifdef _XBOX
// 32-bit accumulator buffer for Xbox: eliminates per-voice clipping distortion
static int MV_Accum32[ MixBufferSize * 2 ];  // stereo int32 pairs (front in surround mode)

/* 5.1 surround mode */
static int MV_SurroundMode = 0;
static int MV_AccumCenter[ MixBufferSize * 2 ];   // C/LFE int32 pairs
static int MV_AccumSurround[ MixBufferSize * 2 ]; // SL/SR int32 pairs
static short MV_CenterMixOut[ MixBufferSize * 2 ];   // center 16-bit output
static short MV_SurroundMixOut[ MixBufferSize * 2 ];  // surround 16-bit output
short *MV_CenterMixBuf = MV_CenterMixOut;
short *MV_SurroundMixBuf = MV_SurroundMixOut;
#endif

static VoiceNode *MV_Voices = NULL;

static volatile VoiceNode VoiceList;
static volatile VoiceNode VoicePool;

static int MV_MixPage      = 0;
static int MV_VoiceHandle  = MV_MinVoiceHandle;

static void ( *MV_CallBackFunc )( unsigned int ) = NULL;
static void ( *MV_RecordFunc )( char *ptr, int length ) = NULL;
static void ( *MV_MixFunction )( VoiceNode *voice, int buffer );

int MV_MaxVolume = 63;

/* Forward declaration — needed before MV_ServiceVoc (surround sweep) */
static short *MV_GetVolumeTable( int vol );

char  *MV_HarshClipTable;
char  *MV_MixDestination;
short *MV_LeftVolume;
short *MV_RightVolume;
int    MV_SampleSize = 1;
int    MV_RightChannelOffset;

unsigned int MV_MixPosition;

int MV_ErrorCode = MV_Ok;

static int lockdepth = 0;
static int DisableInterrupts(void)
{
   if (lockdepth++ > 0) {
      return 0;
   }
   SoundDriver_PCM_Lock();
   return 0;
}

static void RestoreInterrupts(int a)
{
   (void)a;

   if (--lockdepth > 0) {
      return;
   }
   SoundDriver_PCM_Unlock();
}


/*---------------------------------------------------------------------
   Function: MV_ErrorString

   Returns a pointer to the error message associated with an error
   number.  A -1 returns a pointer the current error.
---------------------------------------------------------------------*/

const char *MV_ErrorString
   (
   int ErrorNumber
   )

   {
   const char *ErrorString;

   switch( ErrorNumber )
      {
      case MV_Warning :
      case MV_Error :
         ErrorString = MV_ErrorString( MV_ErrorCode );
         break;

      case MV_Ok :
         ErrorString = "Multivoc ok.";
         break;

      case MV_UnsupportedCard :
         ErrorString = "Selected sound card is not supported by Multivoc.";
         break;

      case MV_NotInstalled :
         ErrorString = "Multivoc not installed.";
         break;

      case MV_DriverError :
         ErrorString = SoundDriver_PCM_ErrorString(SoundDriver_PCM_GetError());
         break;

      case MV_NoVoices :
         ErrorString = "No free voices available to Multivoc.";
         break;

      case MV_NoMem :
         ErrorString = "Out of memory in Multivoc.";
         break;

      case MV_VoiceNotFound :
         ErrorString = "No voice with matching handle found.";
         break;

      case MV_InvalidVOCFile :
         ErrorString = "Invalid VOC file passed in to Multivoc.";
         break;

      case MV_InvalidWAVFile :
         ErrorString = "Invalid WAV file passed in to Multivoc.";
         break;

      case MV_InvalidVorbisFile :
         ErrorString = "Invalid OggVorbis file passed in to Multivoc.";
         break;

      case MV_InvalidMixMode :
         ErrorString = "Invalid mix mode request in Multivoc.";
         break;

      case MV_NullRecordFunction :
         ErrorString = "Null record function passed to MV_StartRecording.";
         break;

      default :
         ErrorString = "Unknown Multivoc error code.";
         break;
      }

   return( ErrorString );
   }


/*---------------------------------------------------------------------
   Function: MV_Mix

   Mixes the sound into the buffer.
---------------------------------------------------------------------*/

static void MV_Mix
   (
   VoiceNode *voice,
   int        buffer
   )

   {
   char          *start;
   int            length;
   int            voclength;
   unsigned int   position;
   unsigned int   rate;
   unsigned int   FixedPointBufferSize;
#ifdef _XBOX
   /* Surround mode: track 3 destination pointers for 3-pass mixing */
   char *dest_front, *dest_center, *dest_surround;
#endif

   if ( ( voice->length == 0 ) && ( voice->GetSound( voice ) != KeepPlaying ) )
      {
      return;
      }


   length               = MixBufferSize;
   FixedPointBufferSize = voice->FixedPointBufferSize;

#ifdef _XBOX
   dest_front    = (char *) MV_Accum32;
   dest_center   = (char *) MV_AccumCenter;
   dest_surround = (char *) MV_AccumSurround;

   if ( !MV_SurroundMode )
      {
      MV_MixDestination = dest_front;
      MV_LeftVolume     = voice->LeftVolume;
      MV_RightVolume    = voice->RightVolume;

      if ( ( MV_Channels == 2 ) && ( IS_QUIET( MV_LeftVolume ) ) )
         {
         MV_LeftVolume      = MV_RightVolume;
         MV_MixDestination += sizeof(int);
         }
      }
#else
   MV_MixDestination    = MV_MixBuffer[ buffer ];
   MV_LeftVolume        = voice->LeftVolume;
   MV_RightVolume       = voice->RightVolume;

   if ( ( MV_Channels == 2 ) && ( IS_QUIET( MV_LeftVolume ) ) )
      {
      MV_LeftVolume      = MV_RightVolume;
      MV_MixDestination += MV_RightChannelOffset;
      }
#endif

   // Add this voice to the mix
   while( length > 0 )
      {
      start    = voice->sound;
      rate     = voice->RateScale;
      position = voice->position;

      // Check if the last sample in this buffer would be
      // beyond the length of the sample block
      if ( ( position + FixedPointBufferSize ) >= voice->length )
         {
         if ( position < voice->length )
            {
            voclength = ( voice->length - position + rate - voice->channels ) / rate;
            }
         else
            {
            voice->GetSound( voice );
            return;
            }
         }
      else
         {
         voclength = length;
         }


#ifdef _XBOX_APU_MIXBUF_WORKING
      // DISABLED: VP doesn't write to GP XMEM MIXBUF on real hardware.
      if ( MV_ApuInitialized && voice->apu_voice >= 0 && voice->apu_started )
         {
         MV_MixPosition = position + rate * voclength;
         }
      else
#endif
      if (voice->mix) {
#ifdef _XBOX
         if ( MV_SurroundMode )
            {
            /* 3-pass surround mixing: reuse existing stereo mix functions
             * with different volume tables and destination buffers. */
            char *saved_front, *saved_center, *saved_surround;

            /* --- Pass 1: Front (FL/FR) --- */
            MV_MixDestination = dest_front;
            MV_LeftVolume     = voice->FLVolume;
            MV_RightVolume    = voice->FRVolume;
            voice->mix( position, rate, start, voclength );
            saved_front = MV_MixDestination;

            /* --- Pass 2: Center / LFE --- */
            MV_MixDestination = dest_center;
            MV_LeftVolume     = voice->CenterVolume;
            MV_RightVolume    = voice->LFEVolume;
            voice->mix( position, rate, start, voclength );
            saved_center = MV_MixDestination;

            /* --- Pass 3: Surround (SL/SR) --- */
            MV_MixDestination = dest_surround;
            MV_LeftVolume     = voice->SLVolume;
            MV_RightVolume    = voice->SRVolume;
            voice->mix( position, rate, start, voclength );
            saved_surround = MV_MixDestination;

            dest_front    = saved_front;
            dest_center   = saved_center;
            dest_surround = saved_surround;
            }
         else
         {
         // Fade-in ramp: save accumulator before mixing, then scale this
         // voice's contribution for the first MV_RAMP_SAMPLES samples.
         // Eliminates click transients when voices start abruptly.
         #define MV_RAMP_SAMPLES 64
         if ( voice->ramp_count < MV_RAMP_SAMPLES )
            {
            int *accum = (int *) MV_MixDestination;
            int ramp_this = MV_RAMP_SAMPLES - voice->ramp_count;
            int save_count = ( voclength < ramp_this ) ? voclength : ramp_this;
            int saved[ MV_RAMP_SAMPLES * 2 ];
            int si;

            // Save accumulator state before this voice mixes in
            for ( si = 0; si < save_count * 2; si++ )
               saved[si] = accum[si];

            voice->mix( position, rate, start, voclength );

            // Apply fade-in ramp to this voice's contribution
            for ( si = 0; si < save_count; si++ )
               {
               int fade = ( ( voice->ramp_count + si ) * 256 ) / MV_RAMP_SAMPLES;
               int dl = accum[ si * 2 ]     - saved[ si * 2 ];
               int dr = accum[ si * 2 + 1 ] - saved[ si * 2 + 1 ];
               accum[ si * 2 ]     = saved[ si * 2 ]     + ( dl * fade >> 8 );
               accum[ si * 2 + 1 ] = saved[ si * 2 + 1 ] + ( dr * fade >> 8 );
               }

            voice->ramp_count += voclength;
            }
         else
#endif
            {
            voice->mix( position, rate, start, voclength );
            }
#ifdef _XBOX
         }
#endif
      }

      voice->position = MV_MixPosition;

      length -= voclength;

      if ( voice->position >= voice->length )
         {
         // Get the next block of sound
         if ( voice->GetSound( voice ) != KeepPlaying )
            {
            return;
            }

         if ( length > (voice->channels - 1) )
            {
            // Get the position of the last sample in the buffer
            FixedPointBufferSize = voice->RateScale * ( length - voice->channels );
            }
         }
      }
   }


/*---------------------------------------------------------------------
   Function: MV_PlayVoice

   Adds a voice to the play list.
---------------------------------------------------------------------*/

void MV_PlayVoice
   (
   VoiceNode *voice
   )

   {
   int flags;

   flags = DisableInterrupts();
#ifdef _XBOX
   voice->ramp_count = 0;
#endif

#ifdef _XBOX_APU
   // Mark APU voice as pending start — actual XApuVoicePlay is deferred
   // to MV_ServiceVoc after GetSound() has parsed the format header and
   // populated voice->sound with real PCM data. (VOC files set BlockLength=0
   // here; actual data isn't available until MV_GetNextVOCBlock runs.)
   if ( MV_ApuInitialized && voice->apu_voice >= 0 )
      {
      voice->apu_started = 0;
      }
#endif

   LL_SortedInsertion( &VoiceList, voice, prev, next, VoiceNode, priority );

   RestoreInterrupts( flags );
   }


/*---------------------------------------------------------------------
   Function: MV_StopVoice

   Removes the voice from the play list and adds it to the free list.
---------------------------------------------------------------------*/

static void MV_StopVoice
   (
   VoiceNode *voice
   )

   {
   int flags;

   flags = DisableInterrupts();

#ifdef _XBOX_APU
   if ( MV_ApuInitialized && voice->apu_voice >= 0 )
      {
      XApuVoiceStop( voice->apu_voice );
      XApuVoiceFree( voice->apu_voice );
      voice->apu_voice = -1;
      voice->apu_started = 0;
      }
#endif

   // move the voice from the play list to the free list
   LL_Remove( voice, next, prev );
   LL_Add( (VoiceNode*) &VoicePool, voice, next, prev );

   RestoreInterrupts( flags );

   #ifdef HAVE_VORBIS
   if (voice->wavetype == Vorbis)
      {
      MV_ReleaseVorbisVoice(voice);
      }
   #endif
   }


/*---------------------------------------------------------------------
   Function: MV_ServiceVoc

   Starts playback of the waiting buffer and mixes the next one.

   JBF: no synchronisation happens inside MV_ServiceVoc nor the
        supporting functions it calls. This would cause a deadlock
        between the mixer thread in the driver vs the nested
        locking in the user-space functions of MultiVoc. The call
        to MV_ServiceVoc is synchronised in the driver.

        Known functions called by MV_ServiceVoc and its helpers:
           MV_Mix (and its MV_Mix*bit* workers)
           MV_GetNextVOCBlock
           MV_GetNextWAVBlock
           MV_SetVoiceMixMode
---------------------------------------------------------------------*/
static void MV_ServiceVoc
   (
   void
   )

   {
   VoiceNode *voice;
   VoiceNode *next;
   //int        flags;

   // Toggle which buffer we'll mix next
   MV_MixPage++;
   if ( MV_MixPage >= MV_NumberOfBuffers )
      {
      MV_MixPage -= MV_NumberOfBuffers;
      }

   if ( MV_ReverbLevel == 0 )
      {
      // Initialize buffer
      //Commented out so that the buffer is always cleared.
      //This is so the guys at Echo Speech can mix into the
      //buffer even when no sounds are playing.
      //if ( !MV_BufferEmpty[ MV_MixPage ] )
         {
         ClearBuffer_DW( MV_MixBuffer[ MV_MixPage ], MV_Silence, MV_BufferSize >> 2 );
         MV_BufferEmpty[ MV_MixPage ] = TRUE;
         }
      }
   else
      {
      char *end;
      char *source;
      char *dest;
      unsigned int   count;
      unsigned int   length;

      end = MV_MixBuffer[ 0 ] + MV_BufferLength;;
      dest = MV_MixBuffer[ MV_MixPage ];
      source = MV_MixBuffer[ MV_MixPage ] - MV_ReverbDelay;
      if ( source < MV_MixBuffer[ 0 ] )
         {
         source += MV_BufferLength;
         }

      length = MV_BufferSize;
      while( length > 0 )
         {
         count = length;
         if ( source + count > end )
            {
            count = (unsigned int)(end - source);
            }

         if ( MV_Bits == 16 )
            {
            if ( MV_ReverbTable != NULL )
               {
               MV_16BitReverb( source, dest, MV_ReverbTable, count / 2 );
               }
            else
               {
               MV_16BitReverbFast( source, dest, count / 2, MV_ReverbLevel );
               }
            }
         else
            {
            if ( MV_ReverbTable != NULL )
               {
               MV_8BitReverb( (signed char *) source, (signed char *) dest, MV_ReverbTable, count );
               }
            else
               {
               MV_8BitReverbFast( (signed char *) source, (signed char *) dest, count, MV_ReverbLevel );
               }
            }

         // if we go through the loop again, it means that we've wrapped around the buffer
         source  = MV_MixBuffer[ 0 ];
         dest   += count;
         length -= count;
         }
      }

   // Play any waiting voices
   //flags = DisableInterrupts();

#ifdef _XBOX
   // Widen 16-bit mix buffer to 32-bit accumulator
   {
      int i, count = MixBufferSize * MV_Channels;
      if ( MV_ReverbLevel == 0 )
         {
         memset( MV_Accum32, 0, count * sizeof(int) );
         }
      else
         {
         short *src16 = (short *) MV_MixBuffer[ MV_MixPage ];
         for ( i = 0; i < count; i++ )
            MV_Accum32[i] = src16[i];
         }

      /* Center/surround accumulators must ALWAYS be cleared — reverb only
       * applies to the front stereo buffer, not surround channels. Without
       * this, stale data accumulates and causes front→surround bleed. */
      if ( MV_SurroundMode )
         {
         memset( MV_AccumCenter, 0, count * sizeof(int) );
         memset( MV_AccumSurround, 0, count * sizeof(int) );
         }
   }

   {
      static int svc_count = 0;
      int vcount = 0;
      VoiceNode *v;
      svc_count++;
      for (v = VoiceList.next; v != &VoiceList; v = v->next) vcount++;
      if (svc_count <= 10)
         xbox_log("MV_ServiceVoc #%d: voices=%d page=%d\n", svc_count, vcount, MV_MixPage);
   }
#endif

#ifdef _XBOX_APU
   // Periodic APU diagnostic dump (every ~10 seconds at ~94 calls/sec)
   {
      static int apu_diag_count = 0;
      apu_diag_count++;
      if (apu_diag_count == 1)
         {
         char diag[16384];
         int diag_len = XApuDiagnostic(diag, sizeof(diag));
         xbox_log("=== APU DIAG #%d ===\n", apu_diag_count);
         extern void xbox_log_write(const char *, int);
         xbox_log_write(diag, diag_len);
         }
   }
#endif

   for( voice = VoiceList.next; voice != &VoiceList; voice = next )
      {
      if ( voice->Paused )
         {
         next = voice->next;
         continue;
         }

      MV_BufferEmpty[ MV_MixPage ] = FALSE;

#ifdef _XBOX_APU
      // Deferred APU voice start: trigger hardware voice when first serviced
      if ( MV_ApuInitialized && voice->apu_voice >= 0 && !voice->apu_started )
         {
         // Make sure sound data is available
         if ( ( voice->length == 0 ) && ( voice->GetSound( voice ) != KeepPlaying ) )
            {
            next = voice->next;
            continue;
            }
         {
         unsigned int total_frames = ( voice->length >> 16 ) + voice->BlockLength;
         unsigned int total_bytes = total_frames * voice->channels * ( voice->bits / 8 );
         int loop = ( voice->LoopStart != NULL ) ? 1 : 0;
         int left_vol = 255, right_vol = 255;

         if ( !IS_QUIET( voice->LeftVolume ) )
            {
            int idx = (int)( voice->LeftVolume - MV_VolumeTable[0] ) / 256;
            left_vol = idx * 255 / MV_MaxVolume;
            }
         else
            {
            left_vol = 0;
            }
         if ( !IS_QUIET( voice->RightVolume ) )
            {
            int idx = (int)( voice->RightVolume - MV_VolumeTable[0] ) / 256;
            right_vol = idx * 255 / MV_MaxVolume;
            }
         else
            {
            right_vol = 0;
            }

         if ( total_bytes > 0 && voice->sound != NULL )
            {
            {
               static int play_dbg = 0;
               if (++play_dbg <= 10)
                  xbox_log("APU_PLAY: v=%d snd=%p bytes=%u rate=%u bits=%d ch=%d loop=%d vol=%d/%d\n",
                     voice->apu_voice, (void*)voice->sound, total_bytes,
                     voice->SamplingRate, voice->bits, voice->channels, loop,
                     left_vol, right_vol);
            }
            XApuVoicePlay( voice->apu_voice, voice->sound, total_bytes,
               voice->SamplingRate, voice->bits, voice->channels, loop,
               left_vol, right_vol );
            }
         else
            {
            static int skip_dbg = 0;
            if (++skip_dbg <= 5)
               xbox_log("APU_SKIP: v=%d bytes=%u snd=%p (skipped play)\n",
                  voice->apu_voice, total_bytes, (void*)voice->sound);
            }
         voice->apu_started = 1;
         }
         }
      // APU voice already started — mark software voice as done.
      // Hardware plays independently; free the software voice node.
      if ( MV_ApuInitialized && voice->apu_voice >= 0 && voice->apu_started )
         {
         voice->Playing = FALSE;
         }
      // Skip software mix when APU hardware mixing is active.
      if ( !MV_ApuInitialized )
#endif
      {
#ifdef _XBOX
      /* Update surround sweep: crossfade SL→SR over the voice's duration.
       * Each ServiceVoc tick mixes MixBufferSize bytes (~1.45ms at 44100Hz).
       * We sweep over ~90 ticks (~2 seconds) from full SL to full SR. */
      if ( MV_SurroundMode && voice->surround_sweep )
         {
         #define SWEEP_TICKS 90
         int t = voice->sweep_ticks;
         int level = 200;
         int sl, sr;
         if ( t >= SWEEP_TICKS ) t = SWEEP_TICKS;
         sr = level * t / SWEEP_TICKS;
         sl = level - sr;
         voice->SLVolume = MV_GetVolumeTable( sl );
         voice->SRVolume = MV_GetVolumeTable( sr );
         voice->sweep_ticks++;
         #undef SWEEP_TICKS
         }
#endif
      MV_MixFunction( voice, MV_MixPage );
      }

      next = voice->next;

      // Is this voice done?
      if ( !voice->Playing )
         {
         //JBF: prevent a deadlock caused by MV_StopVoice grabbing the mutex again
         //MV_StopVoice( voice );
         LL_Remove( voice, next, prev );
         LL_Add( (VoiceNode*) &VoicePool, voice, next, prev );

         if ( MV_CallBackFunc )
            {
            MV_CallBackFunc( voice->callbackval );
            }
         }
      }

   #ifdef _XBOX
   // Convert 32-bit accumulator back to 16-bit with smooth soft limiting.
   // Uses a rational curve above the threshold: no hard knee, derivative is
   // continuous at the transition.  output = T + R*over/(over+R) where
   // T=20000 (threshold), R=12767 (headroom to 32767), over=abs(s)-T.
   // At threshold: gain=1.0 (smooth).  Asymptotically approaches +-32767.
   {
      int i, count = MixBufferSize * MV_Channels;
      short *out = (short *) MV_MixBuffer[ MV_MixPage ];

      #define SOFT_LIMIT(s) do { \
         s >>= 1; \
         if ( s > 20000 ) { int over = s - 20000; s = 20000 + (int)((long long)over * 12767 / (over + 12767)); } \
         else if ( s < -20000 ) { int over = -s - 20000; s = -(20000 + (int)((long long)over * 12767 / (over + 12767))); } \
      } while(0)

      for ( i = 0; i < count; i++ )
         {
         int s = MV_Accum32[i];
         SOFT_LIMIT(s);
         out[i] = (short) s;
         }

      if ( MV_SurroundMode )
         {
         /* Convert center and surround accumulators to 16-bit output buffers */
         for ( i = 0; i < count; i++ )
            {
            int c = MV_AccumCenter[i];
            int r = MV_AccumSurround[i];
            SOFT_LIMIT(c);
            SOFT_LIMIT(r);
            MV_CenterMixOut[i] = (short) c;
            MV_SurroundMixOut[i] = (short) r;
            }
         }
      #undef SOFT_LIMIT
   }
   #endif

#ifdef _XBOX_APU
   // With DS GP program, audio flows through hardware pipeline:
   // VP → MIXBUF → GP (DS program) → EP → AC97 DAC
   // Just call pump for watchdog/diagnostics, output stays zero (SDL silent).
   if ( MV_ApuInitialized )
      {
      XApuPumpMixbuf( NULL, 0 );
      }
#endif

   //RestoreInterrupts(flags);
   }


/*---------------------------------------------------------------------
   Function: MV_GetNextVOCBlock

   Interperate the information of a VOC format sound file.
---------------------------------------------------------------------*/

static playbackstatus MV_GetNextVOCBlock
   (
   VoiceNode *voice
   )

   {
   unsigned char *ptr;
   int            blocktype;
   int            lastblocktype;
   unsigned int   blocklength = 0;
   unsigned int   samplespeed = 0;
   unsigned int   tc = 0;
   int            packtype;
   int            voicemode;
   int            done;
   unsigned       BitsPerSample;
   unsigned       Channels;
   unsigned       Format;

   if ( voice->BlockLength > 0 )
      {
      voice->position    -= voice->length;
      voice->sound       += (voice->length >> 16) * (voice->channels * voice->bits / 8);
      voice->length       = min( voice->BlockLength, 0x8000 );
      voice->BlockLength -= voice->length;
      voice->length     <<= 16;
      return( KeepPlaying );
      }

   if ( ( voice->length > 0 ) && ( voice->LoopEnd != NULL ) &&
      ( voice->LoopStart != NULL ) )
      {
      voice->BlockLength  = voice->LoopSize;
      voice->sound        = voice->LoopStart;
      voice->position     = 0;
      voice->length       = min( voice->BlockLength, 0x8000 );
      voice->BlockLength -= voice->length;
      voice->length     <<= 16;
      return( KeepPlaying );
      }

   ptr = ( unsigned char * )voice->NextBlock;

   voice->Playing = TRUE;

   voicemode = 0;
   lastblocktype = 0;
   packtype = VOC_8BIT;

   done = FALSE;
   while( !done )
      {
      // Stop playing if we get a NULL pointer
      if ( ptr == NULL )
         {
         voice->Playing = FALSE;
         done = TRUE;
         break;
         }

      blocktype = ( int )*ptr;
      blocklength = LITTLE32( *( unsigned int * )( ptr + 1 ) ) & 0x00ffffff;
      ptr += 4;

      switch( blocktype )
         {
         case 0 :
            // End of data
            if ( ( voice->LoopStart == NULL ) ||
               ( voice->LoopStart >= (char *)( ptr - 4 ) ) )
               {
               voice->Playing = FALSE;
               done = TRUE;
               }
            else
               {
               voice->BlockLength  = (unsigned int)((char *)( ptr - 4 ) - voice->LoopStart);
               voice->sound        = voice->LoopStart;
               voice->position     = 0;
               voice->length       = min( voice->BlockLength, 0x8000 );
               voice->BlockLength -= voice->length;
               voice->length     <<= 16;
               return( KeepPlaying );
               }
            break;

         case 1 :
            // Sound data block
            voice->bits  = 8;
            voice->channels = voicemode + 1;
            if ( lastblocktype != 8 )
               {
               tc = ( unsigned int )*ptr << 8;
               packtype = *( ptr + 1 );
               }

            ptr += 2;
            blocklength -= 2;

            samplespeed = 256000000L / ( voice->channels * ( 65536 - tc ) );

            // Skip packed data
            if ( packtype != VOC_8BIT )
               {
               ptr += blocklength;
               }
            else
               {
               done = TRUE;
               }
            voicemode = 0;
            break;

         case 2 :
            // Sound continuation block
            samplespeed = voice->SamplingRate;
            done = TRUE;
            break;

         case 3 :
            // Silence
            // Not implimented.
            ptr += blocklength;
            break;

         case 4 :
            // Marker
            // Not implimented.
            ptr += blocklength;
            break;

         case 5 :
            // ASCII string
            // Not implimented.
            ptr += blocklength;
            break;

         case 6 :
            // Repeat begin
            if ( voice->LoopEnd == NULL )
               {
               voice->LoopCount = LITTLE16(*( unsigned short * )ptr);
               voice->LoopStart = (char *)ptr + blocklength;
               }
            ptr += blocklength;
            break;

         case 7 :
            // Repeat end
            ptr += blocklength;
            if ( lastblocktype == 6 )
               {
               voice->LoopCount = 0;
               }
            else
               {
               if ( ( voice->LoopCount > 0 ) && ( voice->LoopStart != NULL ) )
                  {
                  ptr = (unsigned char *)voice->LoopStart;
                  if ( voice->LoopCount < 0xffff )
                     {
                     voice->LoopCount--;
                     if ( voice->LoopCount == 0 )
                        {
                        voice->LoopStart = NULL;
                        }
                     }
                  }
               }
            break;

         case 8 :
            // Extended block
            voice->bits  = 8;
            voice->channels = 1;
            tc = LITTLE16( *( unsigned short * )ptr );
            packtype = *( ptr + 2 );
            voicemode = *( ptr + 3 );
            ptr += blocklength;
            break;

         case 9 :
            // New sound data block
            samplespeed = LITTLE32( *( unsigned int * )ptr );
            BitsPerSample = ( unsigned )*( ptr + 4 );
            Channels = ( unsigned )*( ptr + 5 );
            Format = ( unsigned )LITTLE16( *( unsigned short * )( ptr + 6 ) );

            if ( ( BitsPerSample == 8 ) && ( Channels == 1 || Channels == 2 ) &&
               ( Format == VOC_8BIT ) )
               {
               ptr         += 12;
               blocklength -= 12;
               voice->bits  = 8;
               voice->channels = Channels;
               done         = TRUE;
               }
            else if ( ( BitsPerSample == 16 ) && ( Channels == 1 || Channels == 2 ) &&
               ( Format == VOC_16BIT ) )
               {
               ptr         += 12;
               blocklength -= 12;
               voice->bits  = 16;
               voice->channels = Channels;
               done         = TRUE;
               }
            else
               {
               ptr += blocklength;
               }
            break;

         default :
            // Unknown data.  Probably not a VOC file.
            voice->Playing = FALSE;
            done = TRUE;
            break;
         }

      lastblocktype = blocktype;
      }

   if ( voice->Playing )
      {
      voice->NextBlock    = (char *)ptr + blocklength;
      voice->sound        = (char *)ptr;

      voice->SamplingRate = samplespeed;
      voice->RateScale    = ( voice->SamplingRate * voice->PitchScale ) / MV_MixRate;

      // Multiply by MixBufferSize - 1
      voice->FixedPointBufferSize = ( voice->RateScale * MixBufferSize ) -
         voice->RateScale;

      if ( voice->LoopEnd != NULL )
         {
         if ( blocklength > (unsigned int)(intptr_t)voice->LoopEnd )
            {
            blocklength = (unsigned int)(intptr_t)voice->LoopEnd;
            }
         else
            {
            voice->LoopEnd = (char *)(intptr_t)blocklength;
            }

         voice->LoopStart = voice->sound + (intptr_t)voice->LoopStart;
         voice->LoopEnd   = voice->sound + (intptr_t)voice->LoopEnd;
         voice->LoopSize  = (unsigned int)(voice->LoopEnd - voice->LoopStart);
         }

      if ( voice->bits == 16 )
         {
         blocklength /= 2;
         }
      if ( voice->channels == 2 )
         {
         blocklength /= 2;
         }

      voice->position     = 0;
      voice->length       = min( blocklength, 0x8000 );
      voice->BlockLength  = blocklength - voice->length;
      voice->length     <<= 16;

      MV_SetVoiceMixMode( voice );

      return( KeepPlaying );
      }

   return( NoMoreData );
   }


/*---------------------------------------------------------------------
   Function: MV_GetNextDemandFeedBlock

   Controls playback of demand fed data.
---------------------------------------------------------------------*/

static playbackstatus MV_GetNextDemandFeedBlock
   (
   VoiceNode *voice
   )

   {
   if ( voice->BlockLength > 0 )
      {
      voice->position    -= voice->length;
      voice->sound       += voice->length >> 16;
      voice->length       = min( voice->BlockLength, 0x8000 );
      voice->BlockLength -= voice->length;
      voice->length     <<= 16;

      return( KeepPlaying );
      }

   if ( voice->DemandFeed == NULL )
      {
      return( NoMoreData );
      }

   voice->position     = 0;
   ( voice->DemandFeed )( &voice->sound, &voice->BlockLength );
   voice->length       = min( voice->BlockLength, 0x8000 );
   voice->BlockLength -= voice->length;
   voice->length     <<= 16;

   if ( ( voice->length > 0 ) && ( voice->sound != NULL ) )
      {
      return( KeepPlaying );
      }
   return( NoMoreData );
   }


/*---------------------------------------------------------------------
   Function: MV_GetNextRawBlock

   Controls playback of demand fed data.
---------------------------------------------------------------------*/

static playbackstatus MV_GetNextRawBlock
   (
   VoiceNode *voice
   )

   {
   if ( voice->BlockLength <= 0 )
      {
      if ( voice->LoopStart == NULL )
         {
         voice->Playing = FALSE;
         return( NoMoreData );
         }

      voice->BlockLength = voice->LoopSize;
      voice->NextBlock   = voice->LoopStart;
      voice->length = 0;
      voice->position = 0;
      }

   voice->sound        = voice->NextBlock;
   voice->position    -= voice->length;
   voice->length       = min( voice->BlockLength, 0x8000 );
   voice->NextBlock   += voice->length * (voice->channels * voice->bits / 8);
   voice->BlockLength -= voice->length;
   voice->length     <<= 16;

   return( KeepPlaying );
   }


/*---------------------------------------------------------------------
   Function: MV_GetNextWAVBlock

   Controls playback of demand fed data.
---------------------------------------------------------------------*/

static playbackstatus MV_GetNextWAVBlock
   (
   VoiceNode *voice
   )

   {
   if ( voice->BlockLength <= 0 )
      {
      if ( voice->LoopStart == NULL )
         {
         voice->Playing = FALSE;
         return( NoMoreData );
         }

      voice->BlockLength = voice->LoopSize;
      voice->NextBlock   = voice->LoopStart;
      voice->length      = 0;
      voice->position    = 0;
      }

   voice->sound        = voice->NextBlock;
   voice->position    -= voice->length;
   voice->length       = min( voice->BlockLength, 0x8000 );
   voice->NextBlock   += voice->length * (voice->channels * voice->bits / 8);
   voice->BlockLength -= voice->length;
   voice->length     <<= 16;

   return( KeepPlaying );
   }


/*---------------------------------------------------------------------
   Function: MV_ServiceRecord

   Starts recording of the waiting buffer.
---------------------------------------------------------------------*/

/*static void MV_ServiceRecord
   (
   void
   )

   {
   if ( MV_RecordFunc )
      {
      MV_RecordFunc( MV_MixBuffer[ 0 ] + MV_MixPage * MixBufferSize,
         MixBufferSize );
      }

   // Toggle which buffer we'll mix next
   MV_MixPage++;
   if ( MV_MixPage >= NumberOfBuffers )
      {
      MV_MixPage = 0;
      }
   }*/


/*---------------------------------------------------------------------
   Function: MV_GetVoice

   Locates the voice with the specified handle.
---------------------------------------------------------------------*/

static VoiceNode *MV_GetVoice
   (
   int handle
   )

   {
   VoiceNode *voice;
   int        flags;

   flags = DisableInterrupts();

   for( voice = VoiceList.next; voice != &VoiceList; voice = voice->next )
      {
      if ( handle == voice->handle )
         {
         break;
         }
      }

   RestoreInterrupts( flags );

   if ( voice == &VoiceList )
      {
      MV_SetErrorCode( MV_VoiceNotFound );
      voice = 0;
      }

   return( voice );
   }


/*---------------------------------------------------------------------
   Function: MV_VoicePlaying

   Checks if the voice associated with the specified handle is
   playing.
---------------------------------------------------------------------*/

int MV_VoicePlaying
   (
   int handle
   )

   {
   VoiceNode *voice;

   if ( !MV_Installed )
      {
      MV_SetErrorCode( MV_NotInstalled );
      return( FALSE );
      }

   voice = MV_GetVoice( handle );

   if ( voice == NULL )
      {
      return( FALSE );
      }

   return( TRUE );
   }


/*---------------------------------------------------------------------
Function: MV_VoicePaused

Checks if the voice associated with the specified handle is
paused.
---------------------------------------------------------------------*/

int MV_VoicePaused
   (
   int handle
   )

   {
   VoiceNode *voice;

   if ( !MV_Installed )
      {
      MV_SetErrorCode( MV_NotInstalled );
      return( FALSE );
      }

   voice = MV_GetVoice( handle );

   if ( voice == NULL )
      {
      return( FALSE );
      }

   return voice->Paused;
   }


/*---------------------------------------------------------------------
   Function: MV_KillAllVoices

   Stops output of all currently active voices.
---------------------------------------------------------------------*/

int MV_KillAllVoices
   (
   void
   )

   {
   VoiceNode * voice, * next;
   int        flags;

   if ( !MV_Installed )
      {
      MV_SetErrorCode( MV_NotInstalled );
      return( MV_Error );
      }

   flags = DisableInterrupts();

   // Remove all the voices from the list
   for( voice = VoiceList.next; voice != &VoiceList; voice = next )
      {
      next = voice->next;
      if (voice->priority < MV_MUSIC_PRIORITY)
         {
         MV_Kill( voice->handle );
         }
      }

   RestoreInterrupts(flags);

   return( MV_Ok );
   }


/*---------------------------------------------------------------------
   Function: MV_Kill

   Stops output of the voice associated with the specified handle.
---------------------------------------------------------------------*/

int MV_Kill
   (
   int handle
   )

   {
   VoiceNode *voice;
   int        flags;
   unsigned int callbackval;

   if ( !MV_Installed )
      {
      MV_SetErrorCode( MV_NotInstalled );
      return( MV_Error );
      }

   flags = DisableInterrupts();

   voice = MV_GetVoice( handle );
   if ( voice == NULL )
      {
      RestoreInterrupts( flags );
      MV_SetErrorCode( MV_VoiceNotFound );
      return( MV_Error );
      }

   callbackval = voice->callbackval;

   MV_StopVoice( voice );

   RestoreInterrupts( flags );

   if ( MV_CallBackFunc )
      {
      MV_CallBackFunc( callbackval );
      }

   return( MV_Ok );
   }


/*---------------------------------------------------------------------
   Function: MV_PauseVoice

   Pauses or resumes output of the voice associated with the specified handle.
---------------------------------------------------------------------*/

int MV_PauseVoice
(
 int handle,
 int pauseon
 )

{
   VoiceNode *voice;
   int        flags;

   if ( !MV_Installed )
   {
      MV_SetErrorCode( MV_NotInstalled );
      return( MV_Error );
   }

   flags = DisableInterrupts();

   voice = MV_GetVoice( handle );
   if ( voice == NULL )
   {
      RestoreInterrupts( flags );
      MV_SetErrorCode( MV_VoiceNotFound );
      return( MV_Error );
   }

   voice->Paused = pauseon;

   RestoreInterrupts( flags );

   return( MV_Ok );
}


/*---------------------------------------------------------------------
   Function: MV_VoicesPlaying

   Determines the number of currently active voices.
---------------------------------------------------------------------*/

int MV_VoicesPlaying
   (
   void
   )

   {
   VoiceNode   *voice;
   int         NumVoices = 0;
   int         flags;

   if ( !MV_Installed )
      {
      MV_SetErrorCode( MV_NotInstalled );
      return( 0 );
      }

   flags = DisableInterrupts();

   for( voice = VoiceList.next; voice != &VoiceList; voice = voice->next )
      {
      NumVoices++;
      }

   RestoreInterrupts( flags );

   return( NumVoices );
   }


/*---------------------------------------------------------------------
   Function: MV_AllocVoice

   Retrieve an inactive or lower priority voice for output.
---------------------------------------------------------------------*/

VoiceNode *MV_AllocVoice
   (
   int priority
   )

   {
   VoiceNode   *voice;
   VoiceNode   *node;
   int          flags;

//return( NULL );
   if ( MV_Recording )
      {
      return( NULL );
      }

   flags = DisableInterrupts();

   // Check if we have any free voices
   if ( LL_Empty( &VoicePool, next, prev ) )
      {
      // check if we have a higher priority than a voice that is playing.
      voice = VoiceList.next;
      for( node = voice->next; node != &VoiceList; node = node->next )
         {
         if ( node->priority < voice->priority )
            {
            voice = node;
            }
         }

      if ( priority >= voice->priority )
         {
         MV_Kill( voice->handle );
         }
      }

   // Check if any voices are in the voice pool
   if ( LL_Empty( &VoicePool, next, prev ) )
      {
      // No free voices
      RestoreInterrupts( flags );
      return( NULL );
      }

   voice = VoicePool.next;
   LL_Remove( voice, next, prev );
   RestoreInterrupts( flags );

   // Find a free voice handle — wrap at a bounded range to avoid
   // unbounded growth (handle reached 43000+ in long play sessions)
   do
      {
      MV_VoiceHandle++;
      if ( MV_VoiceHandle < MV_MinVoiceHandle ||
           MV_VoiceHandle > MV_MinVoiceHandle + MV_MaxVoices * 64 )
         {
         MV_VoiceHandle = MV_MinVoiceHandle;
         }
      }
   while( MV_VoicePlaying( MV_VoiceHandle ) );

   voice->handle = MV_VoiceHandle;

#ifdef _XBOX
   /* Reset surround state from previous voice — stale flags cause bleed */
   voice->is_center      = 0;
   voice->surround_sweep = 0;
   voice->sweep_ticks    = 0;
   voice->FLVolume       = &MV_VolumeTable[ 0 ];
   voice->FRVolume       = &MV_VolumeTable[ 0 ];
   voice->CenterVolume   = &MV_VolumeTable[ 0 ];
   voice->LFEVolume      = &MV_VolumeTable[ 0 ];
   voice->SLVolume       = &MV_VolumeTable[ 0 ];
   voice->SRVolume       = &MV_VolumeTable[ 0 ];
#endif

#ifdef _XBOX_APU
   // Try to allocate an APU hardware voice
   voice->apu_voice = -1;
   voice->apu_started = 0;
   if ( MV_ApuInitialized )
      {
      voice->apu_voice = XApuVoiceAlloc();
      {
         static int alloc_dbg = 0;
         if (++alloc_dbg <= 10)
            xbox_log("APU_ALLOC: voice=%d apu_v=%d\n", voice->handle, voice->apu_voice);
      }
      }
#endif

   return( voice );
   }


/*---------------------------------------------------------------------
   Function: MV_VoiceAvailable

   Checks if a voice can be play at the specified priority.
---------------------------------------------------------------------*/

int MV_VoiceAvailable
   (
   int priority
   )

   {
   VoiceNode   *voice;
   VoiceNode   *node;
   int          flags;

   // Check if we have any free voices
   if ( !LL_Empty( &VoicePool, next, prev ) )
      {
      return( TRUE );
      }

   flags = DisableInterrupts();

   // check if we have a higher priority than a voice that is playing.
   voice = VoiceList.next;
   for( node = VoiceList.next; node != &VoiceList; node = node->next )
      {
      if ( node->priority < voice->priority )
         {
         voice = node;
         }
      }

   RestoreInterrupts( flags );

   if ( ( voice != &VoiceList ) && ( priority >= voice->priority ) )
      {
      return( TRUE );
      }

   return( FALSE );
   }


/*---------------------------------------------------------------------
   Function: MV_SetVoicePitch

   Sets the pitch for the specified voice.
---------------------------------------------------------------------*/

static void MV_SetVoicePitch
   (
   VoiceNode *voice,
   unsigned int rate,
   int pitchoffset
   )

   {
   voice->SamplingRate = rate;
   voice->PitchScale   = PITCH_GetScale( pitchoffset );
   voice->RateScale    = ( rate * voice->PitchScale ) / MV_MixRate;

   // Multiply by MixBufferSize - 1
   voice->FixedPointBufferSize = ( voice->RateScale * MixBufferSize ) -
      voice->RateScale;

#ifdef _XBOX_APU
   if ( MV_ApuInitialized && voice->apu_voice >= 0 )
      {
      // Effective rate after pitch offset
      unsigned int effective_rate = ( rate * voice->PitchScale ) >> 16;
      if ( effective_rate == 0 ) effective_rate = rate;
      XApuVoiceSetPitch( voice->apu_voice, effective_rate );
      }
#endif
   }


/*---------------------------------------------------------------------
   Function: MV_SetPitch

   Sets the pitch for the voice associated with the specified handle.
---------------------------------------------------------------------*/

int MV_SetPitch
   (
   int handle,
   int pitchoffset
   )

   {
   VoiceNode *voice;

   if ( !MV_Installed )
      {
      MV_SetErrorCode( MV_NotInstalled );
      return( MV_Error );
      }

   voice = MV_GetVoice( handle );
   if ( voice == NULL )
      {
      MV_SetErrorCode( MV_VoiceNotFound );
      return( MV_Error );
      }

   MV_SetVoicePitch( voice, voice->SamplingRate, pitchoffset );

   return( MV_Ok );
   }


/*---------------------------------------------------------------------
   Function: MV_SetFrequency

   Sets the frequency for the voice associated with the specified handle.
---------------------------------------------------------------------*/

int MV_SetFrequency
   (
   int handle,
   int frequency
   )

   {
   VoiceNode *voice;

   if ( !MV_Installed )
      {
      MV_SetErrorCode( MV_NotInstalled );
      return( MV_Error );
      }

   voice = MV_GetVoice( handle );
   if ( voice == NULL )
      {
      MV_SetErrorCode( MV_VoiceNotFound );
      return( MV_Error );
      }

   MV_SetVoicePitch( voice, frequency, 0 );

   return( MV_Ok );
   }


/*---------------------------------------------------------------------
   Function: MV_GetFrequency

   Gets the frequency for the voice associated with the specified handle.
---------------------------------------------------------------------*/

int MV_GetFrequency
   (
   int handle,
   int *frequency
   )

   {
   VoiceNode *voice;

   if ( !MV_Installed )
      {
      MV_SetErrorCode( MV_NotInstalled );
      return( MV_Error );
      }

   voice = MV_GetVoice( handle );
   if ( voice == NULL )
      {
      MV_SetErrorCode( MV_VoiceNotFound );
      return( MV_Error );
      }

    if ( voice->SamplingRate == 0 )
      {
      voice->GetSound( voice );
      }

   *frequency = voice->SamplingRate;

   return( MV_Ok );
   }


/*---------------------------------------------------------------------
   Function: MV_GetVolumeTable

   Returns a pointer to the volume table associated with the specified
   volume.
---------------------------------------------------------------------*/

static short *MV_GetVolumeTable
   (
   int vol
   )

   {
   int volume;
   short *table;

   volume = MIX_VOLUME( vol );

   table = (short *) &MV_VolumeTable[ volume ];

   return( table );
   }


/*---------------------------------------------------------------------
   Function: MV_SetVoiceMixMode

   Selects which method should be used to mix the voice.

 8Bit  16Bit  8Bit  16Bit |  8Bit  16Bit  8Bit  16Bit |
 Mono  Mono   Ster  Ster  |  Mono  Mono   Ster  Ster  |  Mixer
 Out   Out    Out   Out   |  In    In     In    In    |
--------------------------+---------------------------+-------------
  X                       |         X                 | Mix8BitMono16
  X                       |   X                       | Mix8BitMono
               X          |         X                 | Mix8BitStereo16
               X          |   X                       | Mix8BitStereo
        X                 |         X                 | Mix16BitMono16
        X                 |   X                       | Mix16BitMono
                     X    |         X                 | Mix16BitStereo16
                     X    |   X                       | Mix16BitStereo
--------------------------+---------------------------+-------------
                     X    |                      X    | Mix16BitStereo16Stereo
                     X    |                X          | Mix16BitStereo8Stereo
               X          |                      X    | Mix8BitStereo16Stereo
               X          |                X          | Mix8BitStereo8Stereo
        X                 |                      X    | Mix16BitMono16Stereo
        X                 |                X          | Mix16BitMono8Stereo
  X                       |                      X    | Mix8BitMono16Stereo
  X                       |                X          | Mix8BitMono8Stereo

---------------------------------------------------------------------*/

void MV_SetVoiceMixMode
   (
   VoiceNode *voice
   )

   {
   //int flags;
   int test;

   //flags = DisableInterrupts();

   test = T_DEFAULT;
   if ( MV_Bits == 8 )
      {
      test |= T_8BITS;
      }

   if ( MV_Channels == 1 )
      {
      test |= T_MONO;
      }
   else
      {
#ifdef _XBOX
      /* In surround mode, never optimize to mono — all 3 passes need
       * both L/R channels written by the mix function. */
      if ( !MV_SurroundMode )
#endif
         {
         if ( IS_QUIET( voice->RightVolume ) )
            {
            test |= T_RIGHTQUIET;
            }
         else if ( IS_QUIET( voice->LeftVolume ) )
            {
            test |= T_LEFTQUIET;
            }
         }
      }

   if ( voice->bits == 16 )
      {
      test |= T_16BITSOURCE;
      }

   if ( voice->channels == 2 )
      {
      test |= T_STEREOSOURCE;
      test &= ~(T_RIGHTQUIET | T_LEFTQUIET);
      }

   switch( test )
      {
      case T_8BITS | T_MONO | T_16BITSOURCE :
         voice->mix = MV_Mix8BitMono16;
         break;

      case T_8BITS | T_MONO :
         voice->mix = MV_Mix8BitMono;
         break;

      case T_8BITS | T_16BITSOURCE | T_LEFTQUIET :
         MV_LeftVolume = MV_RightVolume;
         voice->mix = MV_Mix8BitMono16;
         break;

      case T_8BITS | T_LEFTQUIET :
         MV_LeftVolume = MV_RightVolume;
         voice->mix = MV_Mix8BitMono;
         break;

      case T_8BITS | T_16BITSOURCE | T_RIGHTQUIET :
         voice->mix = MV_Mix8BitMono16;
         break;

      case T_8BITS | T_RIGHTQUIET :
         voice->mix = MV_Mix8BitMono;
         break;

      case T_8BITS | T_16BITSOURCE :
         voice->mix = MV_Mix8BitStereo16;
         break;

      case T_8BITS :
         voice->mix = MV_Mix8BitStereo;
         break;

      case T_MONO | T_16BITSOURCE :
         voice->mix = MV_Mix16BitMono16;
         break;

      case T_MONO :
         voice->mix = MV_Mix16BitMono;
         break;

      case T_16BITSOURCE | T_LEFTQUIET :
         MV_LeftVolume = MV_RightVolume;
         voice->mix = MV_Mix16BitMono16;
         break;

      case T_LEFTQUIET :
         MV_LeftVolume = MV_RightVolume;
         voice->mix = MV_Mix16BitMono;
         break;

      case T_16BITSOURCE | T_RIGHTQUIET :
         voice->mix = MV_Mix16BitMono16;
         break;

      case T_RIGHTQUIET :
         voice->mix = MV_Mix16BitMono;
         break;

      case T_16BITSOURCE :
         voice->mix = MV_Mix16BitStereo16;
         break;

      case T_SIXTEENBIT_STEREO :
         voice->mix = MV_Mix16BitStereo;
         break;

      case T_16BITSOURCE | T_STEREOSOURCE:
         voice->mix = MV_Mix16BitStereo16Stereo;
         break;

      case T_16BITSOURCE | T_STEREOSOURCE | T_8BITS:
         voice->mix = MV_Mix8BitStereo16Stereo;
         break;

      case T_16BITSOURCE | T_STEREOSOURCE | T_MONO:
         voice->mix = MV_Mix16BitMono16Stereo;
         break;

      case T_16BITSOURCE | T_STEREOSOURCE | T_8BITS | T_MONO:
         voice->mix = MV_Mix8BitMono16Stereo;
         break;

      case T_STEREOSOURCE:
         voice->mix = MV_Mix16BitStereo8Stereo;
         break;

      case T_STEREOSOURCE | T_8BITS:
         voice->mix = MV_Mix8BitStereo8Stereo;
         break;

      case T_STEREOSOURCE | T_MONO:
         voice->mix = MV_Mix16BitMono8Stereo;
         break;

      case T_STEREOSOURCE | T_8BITS | T_MONO:
         voice->mix = MV_Mix8BitMono8Stereo;
         break;

      default :
         voice->mix = 0;
      }

   //RestoreInterrupts( flags );
   }


/*---------------------------------------------------------------------
   Function: MV_SetVoiceVolume

   Sets the stereo and mono volume level of the voice associated
   with the specified handle.
---------------------------------------------------------------------*/

void MV_SetVoiceVolume
   (
   VoiceNode *voice,
   int vol,
   int left,
   int right
   )

   {
   if ( MV_Channels == 1 )
      {
      left  = vol;
      right = vol;
      }

   if ( MV_SwapLeftRight )
      {
      // SBPro uses reversed panning
      voice->LeftVolume  = MV_GetVolumeTable( right );
      voice->RightVolume = MV_GetVolumeTable( left );
      }
   else
      {
      voice->LeftVolume  = MV_GetVolumeTable( left );
      voice->RightVolume = MV_GetVolumeTable( right );
      }

#ifdef _XBOX
   /* Default surround volumes: same as stereo L/R for front, silence for rest.
    * MV_Pan3D_Surround overrides these with proper 5.1 panning. */
   if ( MV_SurroundMode )
      {
      if ( voice->surround_sweep )
         {
         /* Sweep voice: surround volumes managed by ServiceVoc tick — don't touch */
         }
      else if ( voice->is_center )
         {
         /* Center channel voice: route to center, silence everything else */
         int center_vol = max( left, right );
         voice->FLVolume      = &MV_VolumeTable[ 0 ];
         voice->FRVolume      = &MV_VolumeTable[ 0 ];
         voice->CenterVolume  = MV_GetVolumeTable( center_vol );
         voice->LFEVolume     = &MV_VolumeTable[ 0 ];
         voice->SLVolume      = &MV_VolumeTable[ 0 ];
         voice->SRVolume      = &MV_VolumeTable[ 0 ];
         }
      else
         {
         /* Default: front stereo, no surround (will be overridden by Pan3D) */
         voice->FLVolume      = voice->LeftVolume;
         voice->FRVolume      = voice->RightVolume;
         voice->CenterVolume  = &MV_VolumeTable[ 0 ];
         voice->LFEVolume     = &MV_VolumeTable[ 0 ];
         voice->SLVolume      = &MV_VolumeTable[ 0 ];
         voice->SRVolume      = &MV_VolumeTable[ 0 ];
         }
      }
#endif

#ifdef _XBOX_APU
   if ( MV_ApuInitialized && voice->apu_voice >= 0 )
      {
      int lv = left, rv = right;
      if ( MV_SwapLeftRight ) { lv = right; rv = left; }
      XApuVoiceSetVolume( voice->apu_voice, lv, rv );
      }
#endif

   MV_SetVoiceMixMode( voice );
   }


/*---------------------------------------------------------------------
   Function: MV_EndLooping

   Stops the voice associated with the specified handle from looping
   without stoping the sound.
---------------------------------------------------------------------*/

int MV_EndLooping
   (
   int handle
   )

   {
   VoiceNode *voice;
   int        flags;

   if ( !MV_Installed )
      {
      MV_SetErrorCode( MV_NotInstalled );
      return( MV_Error );
      }

   flags = DisableInterrupts();

   voice = MV_GetVoice( handle );
   if ( voice == NULL )
      {
      RestoreInterrupts( flags );
      MV_SetErrorCode( MV_VoiceNotFound );
      return( MV_Warning );
      }

   voice->LoopCount = 0;
   voice->LoopStart = NULL;
   voice->LoopEnd   = NULL;

   RestoreInterrupts( flags );

   return( MV_Ok );
   }


/*---------------------------------------------------------------------
   Function: MV_SetPan

   Sets the stereo and mono volume level of the voice associated
   with the specified handle.
---------------------------------------------------------------------*/

int MV_SetPan
   (
   int handle,
   int vol,
   int left,
   int right
   )

   {
   VoiceNode *voice;

   if ( !MV_Installed )
      {
      MV_SetErrorCode( MV_NotInstalled );
      return( MV_Error );
      }

   voice = MV_GetVoice( handle );
   if ( voice == NULL )
      {
      MV_SetErrorCode( MV_VoiceNotFound );
      return( MV_Warning );
      }

   MV_SetVoiceVolume( voice, vol, left, right );

   return( MV_Ok );
   }


/*---------------------------------------------------------------------
   Function: MV_Pan3D

   Set the angle and distance from the listener of the voice associated
   with the specified handle.
---------------------------------------------------------------------*/

int MV_Pan3D
   (
   int handle,
   int angle,
   int distance
   )

   {
   int left;
   int right;
   int mid;
   int volume;
   int status;

   if ( distance < 0 )
      {
      distance  = -distance;
      angle    += MV_NumPanPositions / 2;
      }

   volume = MIX_VOLUME( distance );

   // Ensure angle is within 0 - 31
   angle &= MV_MaxPanPosition;

   left  = MV_PanTable[ angle ][ volume ].left;
   right = MV_PanTable[ angle ][ volume ].right;
   mid   = max( 0, 255 - distance );

   status = MV_SetPan( handle, mid, left, right );

#ifdef _XBOX
   /* After SetPan sets the base stereo volumes, override with 5.1 panning.
    * Game angle 0-31: 0=behind, 8=right, 16=ahead, 24=left.
    * We remap to surround convention then distribute across FL/FR/SL/SR. */
   if ( MV_SurroundMode && status == MV_Ok )
      {
      VoiceNode *voice = MV_GetVoice( handle );
      if ( voice != NULL && !voice->is_center && !voice->surround_sweep )
         {
         int level = max( 0, 255 - distance );
         int fl, fr, sl, sr;

         /* 4-speaker VBAP panning: compute per-speaker gains.
          * Build engine convention: 0=behind, 8=right, 16=ahead, 24=left.
          * Surround code expects:   0=ahead,  8=right, 16=behind, 24=left.
          * Reflect front/back (preserve L/R): sa = (48 - angle) & 31 */
         int sa = ( 48 - angle ) & 31;

         if ( sa <= 8 )
            {
            /* Front-right quadrant (0-8): FL→FR crossfade, no surround */
            fr = level * sa / 8;
            fl = level - fr;
            sl = 0;
            sr = 0;
            }
         else if ( sa <= 16 )
            {
            /* Right-rear quadrant (8-16): FR→SR crossfade */
            sr = level * ( sa - 8 ) / 8;
            fr = level - sr;
            fl = 0;
            sl = 0;
            }
         else if ( sa <= 24 )
            {
            /* Rear-left quadrant (16-24): SR→SL crossfade */
            sl = level * ( sa - 16 ) / 8;
            sr = level - sl;
            fl = 0;
            fr = 0;
            }
         else
            {
            /* Left-front quadrant (24-31): SL→FL crossfade */
            fl = level * ( sa - 24 ) / 8;
            sl = level - fl;
            fr = 0;
            sr = 0;
            }

         voice->FLVolume     = MV_GetVolumeTable( fl );
         voice->FRVolume     = MV_GetVolumeTable( fr );
         voice->CenterVolume = &MV_VolumeTable[ 0 ];
         voice->LFEVolume    = &MV_VolumeTable[ 0 ];
         voice->SLVolume     = MV_GetVolumeTable( sl );
         voice->SRVolume     = MV_GetVolumeTable( sr );
         }
      }
#endif

   return( status );
   }


/*---------------------------------------------------------------------
   Function: MV_SetReverb

   Sets the level of reverb to add to mix.
---------------------------------------------------------------------*/

void MV_SetReverb
   (
   int reverb
   )

   {
   MV_ReverbLevel = MIX_VOLUME( reverb );
   MV_ReverbTable = &MV_VolumeTable[ MV_ReverbLevel ];
   }


/*---------------------------------------------------------------------
   Function: MV_SetFastReverb

   Sets the level of reverb to add to mix.
---------------------------------------------------------------------*/

void MV_SetFastReverb
   (
   int reverb
   )

   {
   MV_ReverbLevel = max( 0, min( 16, reverb ) );
   MV_ReverbTable = NULL;
   }


/*---------------------------------------------------------------------
   Function: MV_GetMaxReverbDelay

   Returns the maximum delay time for reverb.
---------------------------------------------------------------------*/

int MV_GetMaxReverbDelay
   (
   void
   )

   {
   int maxdelay;

   maxdelay = MixBufferSize * MV_NumberOfBuffers;

   return maxdelay;
   }


/*---------------------------------------------------------------------
   Function: MV_GetReverbDelay

   Returns the current delay time for reverb.
---------------------------------------------------------------------*/

int MV_GetReverbDelay
   (
   void
   )

   {
   return MV_ReverbDelay / MV_SampleSize;
   }


/*---------------------------------------------------------------------
   Function: MV_SetReverbDelay

   Sets the delay level of reverb to add to mix.
---------------------------------------------------------------------*/

void MV_SetReverbDelay
   (
   int delay
   )

   {
   int maxdelay;

   maxdelay = MV_GetMaxReverbDelay();
   MV_ReverbDelay = max( MixBufferSize, min( delay, maxdelay ) );
   MV_ReverbDelay *= MV_SampleSize;
   }


/*---------------------------------------------------------------------
   Function: MV_SetMixMode

   Prepares Multivoc to play stereo of mono digitized sounds.
---------------------------------------------------------------------*/

int MV_SetMixMode
   (
   int numchannels,
   int samplebits
   )

   {
   int mode;

   if ( !MV_Installed )
      {
      MV_SetErrorCode( MV_NotInstalled );
      return( MV_Error );
      }

   mode = 0;
   if ( numchannels == 2 )
      {
      mode |= STEREO;
      }
   if ( samplebits == 16 )
      {
      mode |= SIXTEEN_BIT;
      }

   MV_MixMode = mode;

   MV_Channels = 1;
   if ( MV_MixMode & STEREO )
      {
      MV_Channels = 2;
      }

   MV_Bits = 8;
   if ( MV_MixMode & SIXTEEN_BIT )
      {
      MV_Bits = 16;
      }

   MV_BuffShift  = 7 + MV_Channels;
   MV_SampleSize = sizeof( MONO8 ) * MV_Channels;

   if ( MV_Bits == 8 )
      {
      MV_Silence = SILENCE_8BIT;
      }
   else
      {
      MV_Silence     = SILENCE_16BIT;
      MV_BuffShift  += 1;
      MV_SampleSize *= 2;
      }

   MV_BufferSize = MixBufferSize * MV_SampleSize;
   MV_NumberOfBuffers = TotalBufferSize / MV_BufferSize;
   MV_BufferLength = TotalBufferSize;

   MV_RightChannelOffset = MV_SampleSize / 2;

   return( MV_Ok );
   }


/*---------------------------------------------------------------------
   Function: MV_StartPlayback

   Starts the sound playback engine.
---------------------------------------------------------------------*/

int MV_StartPlayback
   (
   void
   )

   {
   int status;
   int buffer;

   // Initialize the buffers
   ClearBuffer_DW( MV_MixBuffer[ 0 ], MV_Silence, TotalBufferSize >> 2 );
   for( buffer = 0; buffer < MV_NumberOfBuffers; buffer++ )
      {
      MV_BufferEmpty[ buffer ] = TRUE;
      }

   // Set the mix buffer variables
   MV_MixPage = 1;

   MV_MixFunction = MV_Mix;

//JIM
//   MV_MixRate = MV_RequestedMixRate;
//   return( MV_Ok );

   // Start playback
   status = SoundDriver_PCM_BeginPlayback(MV_MixBuffer[0], MV_BufferSize,
                                      MV_NumberOfBuffers, MV_ServiceVoc);
   if (status != MV_Ok) {
      MV_SetErrorCode(MV_DriverError);
      return MV_Error;
   }

   MV_MixRate = MV_RequestedMixRate;

   return( MV_Ok );
   }


/*---------------------------------------------------------------------
   Function: MV_StopPlayback

   Stops the sound playback engine.
---------------------------------------------------------------------*/

void MV_StopPlayback
   (
   void
   )

   {
   VoiceNode   *voice;
   VoiceNode   *next;
   int          flags;

   // Stop sound playback
   SoundDriver_PCM_StopPlayback();

   // Make sure all callbacks are done.
   flags = DisableInterrupts();

   for( voice = VoiceList.next; voice != &VoiceList; voice = next )
      {
      next = voice->next;

      MV_StopVoice( voice );

      if ( MV_CallBackFunc )
         {
         MV_CallBackFunc( voice->callbackval );
         }
      }

   RestoreInterrupts( flags );
   }


/*---------------------------------------------------------------------
   Function: MV_StartRecording

   Starts the sound recording engine.
---------------------------------------------------------------------*/

int MV_StartRecording
   (
   int MixRate,
   void ( *function )( char *ptr, int length )
   )

   {
   (void)MixRate; (void)function;

   MV_SetErrorCode( MV_UnsupportedCard );
   return( MV_Error );
   }


/*---------------------------------------------------------------------
   Function: MV_StopRecord

   Stops the sound record engine.
---------------------------------------------------------------------*/

void MV_StopRecord
   (
   void
   )

   {
   }


/*---------------------------------------------------------------------
   Function: MV_StartDemandFeedPlayback

   Plays a digitized sound from a user controlled buffering system.
---------------------------------------------------------------------*/

int MV_StartDemandFeedPlayback
   (
   void ( *function )( char **ptr, unsigned int *length ),
   int rate,
   int pitchoffset,
   int vol,
   int left,
   int right,
   int priority,
   unsigned int callbackval
   )

   {
   VoiceNode *voice;

   if ( !MV_Installed )
      {
      MV_SetErrorCode( MV_NotInstalled );
      return( MV_Error );
      }

   // Request a voice from the voice pool
   voice = MV_AllocVoice( priority );
   if ( voice == NULL )
      {
      MV_SetErrorCode( MV_NoVoices );
      return( MV_Error );
      }

   voice->wavetype    = DemandFeed;
   voice->bits        = 8;
   voice->channels    = 1;
   voice->GetSound    = MV_GetNextDemandFeedBlock;
   voice->NextBlock   = NULL;
   voice->DemandFeed  = function;
   voice->LoopStart   = NULL;
   voice->LoopCount   = 0;
   voice->BlockLength = 0;
   voice->position    = 0;
   voice->sound       = NULL;
   voice->length      = 0;
   voice->BlockLength = 0;
   voice->Playing     = TRUE;
   voice->Paused      = FALSE;
   voice->next        = NULL;
   voice->prev        = NULL;
   voice->priority    = priority;
   voice->callbackval = callbackval;

   MV_SetVoicePitch( voice, rate, pitchoffset );
   MV_SetVoiceVolume( voice, vol, left, right );
   MV_PlayVoice( voice );

   return( voice->handle );
   }


/*---------------------------------------------------------------------
   Function: MV_PlayRaw

   Begin playback of sound data with the given sound levels and
   priority.
---------------------------------------------------------------------*/

int MV_PlayRaw
   (
   char *ptr,
   unsigned int length,
   unsigned rate,
   int   pitchoffset,
   int   vol,
   int   left,
   int   right,
   int   priority,
   unsigned int callbackval
   )

   {
   int status;

   status = MV_PlayLoopedRaw( ptr, length, NULL, NULL, rate, pitchoffset,
      vol, left, right, priority, callbackval );

   return( status );
   }


/*---------------------------------------------------------------------
   Function: MV_PlayLoopedRaw

   Begin playback of sound data with the given sound levels and
   priority.
---------------------------------------------------------------------*/

int MV_PlayLoopedRaw
   (
   char *ptr,
   unsigned int length,
   char *loopstart,
   char *loopend,
   unsigned rate,
   int   pitchoffset,
   int   vol,
   int   left,
   int   right,
   int   priority,
   unsigned int callbackval
   )

   {
   VoiceNode *voice;

   if ( !MV_Installed )
      {
      MV_SetErrorCode( MV_NotInstalled );
      return( MV_Error );
      }

   // Request a voice from the voice pool
   voice = MV_AllocVoice( priority );
   if ( voice == NULL )
      {
      MV_SetErrorCode( MV_NoVoices );
      return( MV_Error );
      }

   voice->wavetype    = Raw;
   voice->bits        = 8;
   voice->channels    = 1;
   voice->GetSound    = MV_GetNextRawBlock;
   voice->Playing     = TRUE;
   voice->Paused      = FALSE;
   voice->NextBlock   = ptr;
   voice->position    = 0;
   voice->BlockLength = length;
   voice->length      = 0;
   voice->next        = NULL;
   voice->prev        = NULL;
   voice->priority    = priority;
   voice->callbackval = callbackval;
   voice->LoopStart   = loopstart;
   voice->LoopEnd     = loopend;
   voice->LoopSize    = (unsigned int)( voice->LoopEnd - voice->LoopStart ) + 1;

   MV_SetVoicePitch( voice, rate, pitchoffset );
   MV_SetVoiceVolume( voice, vol, left, right );
   MV_PlayVoice( voice );

   return( voice->handle );
   }


/*---------------------------------------------------------------------
   Function: MV_PlayWAV

   Begin playback of sound data with the given sound levels and
   priority.
---------------------------------------------------------------------*/

int MV_PlayWAV
   (
   char *ptr,
   unsigned int length,
   int   pitchoffset,
   int   vol,
   int   left,
   int   right,
   int   priority,
   unsigned int callbackval
   )

   {
   int status;

   status = MV_PlayLoopedWAV( ptr, length, -1, -1, pitchoffset, vol, left, right,
      priority, callbackval );

   return( status );
   }


/*---------------------------------------------------------------------
   Function: MV_PlayWAV3D

   Begin playback of sound data at specified angle and distance
   from listener.
---------------------------------------------------------------------*/

int MV_PlayWAV3D
   (
   char *ptr,
   unsigned int length,
   int  pitchoffset,
   int  angle,
   int  distance,
   int  priority,
   unsigned int callbackval
   )

   {
   int left;
   int right;
   int mid;
   int volume;
   int status;

   if ( !MV_Installed )
      {
      MV_SetErrorCode( MV_NotInstalled );
      return( MV_Error );
      }

   if ( distance < 0 )
      {
      distance  = -distance;
      angle    += MV_NumPanPositions / 2;
      }

   volume = MIX_VOLUME( distance );

   // Ensure angle is within 0 - 31
   angle &= MV_MaxPanPosition;

   left  = MV_PanTable[ angle ][ volume ].left;
   right = MV_PanTable[ angle ][ volume ].right;
   mid   = max( 0, 255 - distance );

   status = MV_PlayWAV( ptr, length, pitchoffset, mid, left, right, priority,
      callbackval );

   return( status );
   }


/*---------------------------------------------------------------------
   Function: MV_PlayRaw3D

   Begin playback of sound data at specified angle and distance
   from listener.
---------------------------------------------------------------------*/

int MV_PlayRaw3D
   (
   char *ptr,
   unsigned int length,
   unsigned rate,
   int  pitchoffset,
   int  angle,
   int  distance,
   int  priority,
   unsigned int callbackval
   )

   {
   int left;
   int right;
   int mid;
   int volume;
   int status;

   if ( !MV_Installed )
      {
      MV_SetErrorCode( MV_NotInstalled );
      return( MV_Error );
      }

   if ( distance < 0 )
      {
      distance  = -distance;
      angle    += MV_NumPanPositions / 2;
      }

   volume = MIX_VOLUME( distance );

   // Ensure angle is within 0 - 31
   angle &= MV_MaxPanPosition;

   left  = MV_PanTable[ angle ][ volume ].left;
   right = MV_PanTable[ angle ][ volume ].right;
   mid   = max( 0, 255 - distance );

   status = MV_PlayRaw( ptr, length, rate, pitchoffset, mid, left, right,
      priority, callbackval );

   return( status );
   }


/*---------------------------------------------------------------------
   Function: MV_PlayLoopedWAV

   Begin playback of sound data with the given sound levels and
   priority.
---------------------------------------------------------------------*/

int MV_PlayLoopedWAV
   (
   char *ptr,
   unsigned int ptrlength,
   int   loopstart,
   int   loopend,
   int   pitchoffset,
   int   vol,
   int   left,
   int   right,
   int   priority,
   unsigned int callbackval
   )

   {
   riff_header   riff;
   format_header format;
   data_header   data;
   VoiceNode     *voice;
   char *dataptr = ptr;
   int length;
   int absloopend;
   int absloopstart;
   int sizemask;

   (void)ptrlength;

   if ( !MV_Installed )
      {
      MV_SetErrorCode( MV_NotInstalled );
      return( MV_Error );
      }

   memcpy(&riff, dataptr, sizeof(riff_header));
   riff.file_size   = LITTLE32(riff.file_size);
   riff.format_size = LITTLE32(riff.format_size);
   dataptr += sizeof(riff_header);

   if ( ( memcmp( riff.RIFF, "RIFF", 4 ) != 0 ) ||
      ( memcmp( riff.WAVE, "WAVE", 4 ) != 0 ) ||
      ( memcmp( riff.fmt, "fmt ", 4) != 0 ) )
      {
      MV_SetErrorCode( MV_InvalidWAVFile );
      return( MV_Error );
      }

   memcpy(&format, dataptr, sizeof(format_header));
   format.wFormatTag      = LITTLE16(format.wFormatTag);
   format.nChannels       = LITTLE16(format.nChannels);
   format.nSamplesPerSec  = LITTLE32(format.nSamplesPerSec);
   format.nAvgBytesPerSec = LITTLE32(format.nAvgBytesPerSec);
   format.nBlockAlign     = LITTLE16(format.nBlockAlign);
   format.nBitsPerSample  = LITTLE16(format.nBitsPerSample);
   dataptr += riff.format_size;

   memcpy(&data, dataptr, sizeof(data_header));
   data.size = LITTLE32(data.size);

   // Check if it's PCM data.
   if ( format.wFormatTag != 1 )
      {
      MV_SetErrorCode( MV_InvalidWAVFile );
      return( MV_Error );
      }

   if ( format.nChannels != 1 && format.nChannels != 2 )
      {
      MV_SetErrorCode( MV_InvalidWAVFile );
      return( MV_Error );
      }

   if ( ( format.nBitsPerSample != 8 ) &&
      ( format.nBitsPerSample != 16 ) )
      {
      MV_SetErrorCode( MV_InvalidWAVFile );
      return( MV_Error );
      }

   if ( memcmp( data.DATA, "data", 4 ) != 0 )
      {
      MV_SetErrorCode( MV_InvalidWAVFile );
      return( MV_Error );
      }

   // Request a voice from the voice pool
   voice = MV_AllocVoice( priority );
   if ( voice == NULL )
      {
      MV_SetErrorCode( MV_NoVoices );
      return( MV_Error );
      }

   voice->wavetype    = WAV;
   voice->bits        = format.nBitsPerSample;
   voice->channels    = format.nChannels;
   voice->GetSound    = MV_GetNextWAVBlock;

   length = data.size;
   absloopstart = loopstart;
   absloopend   = loopend;
   sizemask = 0;
   if ( voice->bits == 16 )
      {
      loopstart  *= 2;
      loopend    *= 2;
      length     /= 2;
      sizemask    = 1;
      }
   if ( voice->channels == 2 )
      {
      loopstart  *= 2;
      loopend    *= 2;
      length     /= 2;
      sizemask    = (sizemask<<1) | 1;
      }
   data.size  &= ~sizemask;

   loopend    = (int)min( (unsigned int)loopend, data.size );
   absloopend = (int)min( (unsigned int)absloopend, (unsigned int)length );

   voice->Playing     = TRUE;
   voice->Paused      = FALSE;
   voice->DemandFeed  = NULL;
   voice->LoopStart   = NULL;
   voice->LoopCount   = 0;
   voice->position    = 0;
   voice->length      = 0;
   voice->BlockLength = absloopend;
   voice->NextBlock   = dataptr + sizeof(data_header);
   voice->next        = NULL;
   voice->prev        = NULL;
   voice->priority    = priority;
   voice->callbackval = callbackval;
   voice->LoopStart   = voice->NextBlock + loopstart;
   voice->LoopEnd     = voice->NextBlock + loopend;
   voice->LoopSize    = absloopend - absloopstart;

   if ( ( loopstart >= (int)data.size ) || ( loopstart < 0 ) )
      {
      voice->LoopStart = NULL;
      voice->LoopEnd   = NULL;
      voice->BlockLength = length;
      }

   MV_SetVoicePitch( voice, format.nSamplesPerSec, pitchoffset );
   MV_SetVoiceVolume( voice, vol, left, right );
   MV_PlayVoice( voice );

   return( voice->handle );
   }


/*---------------------------------------------------------------------
   Function: MV_PlayVOC3D

   Begin playback of sound data at specified angle and distance
   from listener.
---------------------------------------------------------------------*/

int MV_PlayVOC3D
   (
   char *ptr,
   unsigned int ptrlength,
   int  pitchoffset,
   int  angle,
   int  distance,
   int  priority,
   unsigned int callbackval
   )

   {
   int left;
   int right;
   int mid;
   int volume;
   int status;

   if ( !MV_Installed )
      {
      MV_SetErrorCode( MV_NotInstalled );
      return( MV_Error );
      }

   if ( distance < 0 )
      {
      distance  = -distance;
      angle    += MV_NumPanPositions / 2;
      }

   volume = MIX_VOLUME( distance );

   // Ensure angle is within 0 - 31
   angle &= MV_MaxPanPosition;

   left  = MV_PanTable[ angle ][ volume ].left;
   right = MV_PanTable[ angle ][ volume ].right;
   mid   = max( 0, 255 - distance );

   status = MV_PlayVOC( ptr, ptrlength, pitchoffset, mid, left, right, priority,
      callbackval );

   return( status );
   }


/*---------------------------------------------------------------------
   Function: MV_PlayVOC

   Begin playback of sound data with the given sound levels and
   priority.
---------------------------------------------------------------------*/

int MV_PlayVOC
   (
   char *ptr,
   unsigned int ptrlength,
   int   pitchoffset,
   int   vol,
   int   left,
   int   right,
   int   priority,
   unsigned int callbackval
   )

   {
   int status;

   status = MV_PlayLoopedVOC( ptr, ptrlength, -1, -1, pitchoffset, vol, left, right,
      priority, callbackval );

   return( status );
   }


/*---------------------------------------------------------------------
   Function: MV_PlayLoopedVOC

   Begin playback of sound data with the given sound levels and
   priority.
---------------------------------------------------------------------*/

int MV_PlayLoopedVOC
   (
   char *ptr,
   unsigned int ptrlength,
   int   loopstart,
   int   loopend,
   int   pitchoffset,
   int   vol,
   int   left,
   int   right,
   int   priority,
   unsigned int callbackval
   )

   {
   VoiceNode   *voice;
   int          status;

   (void)ptrlength;

   if ( !MV_Installed )
      {
      MV_SetErrorCode( MV_NotInstalled );
      return( MV_Error );
      }

   // Make sure it's a valid VOC file.
   status = memcmp( ptr, "Creative Voice File", 19 );
   if ( status != 0 )
      {
      MV_SetErrorCode( MV_InvalidVOCFile );
      return( MV_Error );
      }

   // Request a voice from the voice pool
   voice = MV_AllocVoice( priority );
   if ( voice == NULL )
      {
      MV_SetErrorCode( MV_NoVoices );
      return( MV_Error );
      }

   voice->wavetype    = VOC;
   voice->bits        = 8;
   voice->channels    = 1;
   voice->GetSound    = MV_GetNextVOCBlock;
   voice->NextBlock   = ptr + LITTLE16(*( unsigned short * )( ptr + 0x14 ));
   voice->DemandFeed  = NULL;
   voice->LoopStart   = NULL;
   voice->LoopCount   = 0;
   voice->BlockLength = 0;
   voice->PitchScale  = PITCH_GetScale( pitchoffset );
   voice->length      = 0;
   voice->next        = NULL;
   voice->prev        = NULL;
   voice->priority    = priority;
   voice->callbackval = callbackval;
   voice->LoopStart   = ( char * )(intptr_t)loopstart;
   voice->LoopEnd     = ( char * )(intptr_t)loopend;
   voice->LoopSize    = loopend - loopstart + 1;

   if ( loopstart < 0 )
      {
      voice->LoopStart = NULL;
      voice->LoopEnd   = NULL;
      }

   MV_SetVoiceVolume( voice, vol, left, right );
   MV_PlayVoice( voice );

   return( voice->handle );
   }


/*---------------------------------------------------------------------
   Function: MV_CreateVolumeTable

   Create the table used to convert sound data to a specific volume
   level.
---------------------------------------------------------------------*/

void MV_CreateVolumeTable
   (
   int index,
   int volume,
   int MaxVolume
   )

   {
   int val;
   int level;
   int i;

   level = ( volume * MaxVolume ) / MV_MaxTotalVolume;
   if ( MV_Bits == 16 )
      {
      for( i = 0; i < 65536; i += 256 )
         {
         val   = i - 0x8000;
         val  *= level;
         val  /= MV_MaxVolume;
         MV_VolumeTable[ index ][ i / 256 ] = val;
         }
      }
   else
      {
      for( i = 0; i < 256; i++ )
         {
         val   = i - 0x80;
         val  *= level;
         val  /= MV_MaxVolume;
         MV_VolumeTable[ volume ][ i ] = val;
         }
      }
   }


/*---------------------------------------------------------------------
   Function: MV_CalcVolume

   Create the table used to convert sound data to a specific volume
   level.
---------------------------------------------------------------------*/

static void MV_CalcVolume
   (
   int MaxVolume
   )

   {
   int volume;

   for( volume = 0; volume < 128; volume++ )
      {
      MV_HarshClipTable[ volume ] = 0;
      MV_HarshClipTable[ volume + 384 ] = 255;
      }
   for( volume = 0; volume < 256; volume++ )
      {
      MV_HarshClipTable[ volume + 128 ] = volume;
      }

   // For each volume level, create a translation table with the
   // appropriate volume calculated.
   for( volume = 0; volume <= MV_MaxVolume; volume++ )
      {
      MV_CreateVolumeTable( volume, volume, MaxVolume );
      }
   }


/*---------------------------------------------------------------------
   Function: MV_CalcPanTable

   Create the table used to determine the stereo volume level of
   a sound located at a specific angle and distance from the listener.
---------------------------------------------------------------------*/

static void MV_CalcPanTable
   (
   void
   )

   {
   int   level;
   int   angle;
   int   distance;
   int   HalfAngle;
   int   ramp;

   HalfAngle = ( MV_NumPanPositions / 2 );

   for( distance = 0; distance <= MV_MaxVolume; distance++ )
      {
      level = ( 255 * ( MV_MaxVolume - distance ) ) / MV_MaxVolume;
      for( angle = 0; angle <= HalfAngle / 2; angle++ )
         {
         ramp = level - ( ( level * angle ) /
            ( MV_NumPanPositions / 4 ) );

         MV_PanTable[ angle ][ distance ].left = ramp;
         MV_PanTable[ HalfAngle - angle ][ distance ].left = ramp;
         MV_PanTable[ HalfAngle + angle ][ distance ].left = level;
         MV_PanTable[ MV_MaxPanPosition - angle ][ distance ].left = level;

         MV_PanTable[ angle ][ distance ].right = level;
         MV_PanTable[ HalfAngle - angle ][ distance ].right = level;
         MV_PanTable[ HalfAngle + angle ][ distance ].right = ramp;
         MV_PanTable[ MV_MaxPanPosition - angle ][ distance ].right = ramp;
         }
      }
   }


/*---------------------------------------------------------------------
   Function: MV_SetVolume

   Sets the volume of digitized sound playback.
---------------------------------------------------------------------*/

void MV_SetVolume
   (
   int volume
   )

   {
   volume = max( 0, volume );
   volume = min( volume, MV_MaxTotalVolume );

   MV_TotalVolume = volume;

   // Calculate volume table
   MV_CalcVolume( volume );
   }


/*---------------------------------------------------------------------
   Function: MV_GetVolume

   Returns the volume of digitized sound playback.
---------------------------------------------------------------------*/

int MV_GetVolume
   (
   void
   )

   {
   return( MV_TotalVolume );
   }


/*---------------------------------------------------------------------
   Function: MV_SetCallBack

   Set the function to call when a voice stops.
---------------------------------------------------------------------*/

void MV_SetCallBack
   (
   void ( *function )( unsigned int )
   )

   {
   MV_CallBackFunc = function;
   }


/*---------------------------------------------------------------------
   Function: MV_SetReverseStereo

   Set the orientation of the left and right channels.
---------------------------------------------------------------------*/

void MV_SetReverseStereo
   (
   int setting
   )

   {
   MV_SwapLeftRight = setting;
   }


/*---------------------------------------------------------------------
   Function: MV_GetReverseStereo

   Returns the orientation of the left and right channels.
---------------------------------------------------------------------*/

int MV_GetReverseStereo
   (
   void
   )

   {
   return( MV_SwapLeftRight );
   }


/*---------------------------------------------------------------------
   Function: MV_Init

   Perform the initialization of variables and memory used by
   Multivoc.
---------------------------------------------------------------------*/

int MV_Init
   (
   int soundcard,
   int * MixRate,
   int Voices,
   int * numchannels,
   int * samplebits,
   void * initdata
   )

   {
   char *ptr;
   int  status;
   int  buffer;
   int  index;

   if ( MV_Installed )
      {
      MV_Shutdown();
      }

   MV_SetErrorCode( MV_Ok );

   MV_TotalMemory = Voices * sizeof( VoiceNode ) + sizeof( HARSH_CLIP_TABLE_8 ) + TotalBufferSize;
   ptr = (char *) malloc( MV_TotalMemory );
   if ( !ptr )
      {
      MV_SetErrorCode( MV_NoMem );
      return( MV_Error );
      }

   memset(ptr, 0, MV_TotalMemory);

   MV_Voices = ( VoiceNode * )ptr;
   ptr += Voices * sizeof( VoiceNode );

   MV_HarshClipTable = ptr;
   ptr += sizeof(HARSH_CLIP_TABLE_8);

   // Set number of voices before calculating volume table
   MV_MaxVoices = Voices;

   LL_Reset( (VoiceNode*) &VoiceList, next, prev );
   LL_Reset( (VoiceNode*) &VoicePool, next, prev );

   for( index = 0; index < Voices; index++ )
      {
#ifdef _XBOX
      MV_Voices[ index ].is_center = 0;
      MV_Voices[ index ].surround_sweep = 0;
      MV_Voices[ index ].sweep_ticks = 0;
      MV_Voices[ index ].FLVolume      = &MV_VolumeTable[ 0 ];
      MV_Voices[ index ].FRVolume      = &MV_VolumeTable[ 0 ];
      MV_Voices[ index ].CenterVolume  = &MV_VolumeTable[ 0 ];
      MV_Voices[ index ].LFEVolume     = &MV_VolumeTable[ 0 ];
      MV_Voices[ index ].SLVolume      = &MV_VolumeTable[ 0 ];
      MV_Voices[ index ].SRVolume      = &MV_VolumeTable[ 0 ];
#endif
#ifdef _XBOX_APU
      MV_Voices[ index ].apu_voice = -1;
      MV_Voices[ index ].apu_started = 0;
#endif
      LL_Add( (VoiceNode*) &VoicePool, &MV_Voices[ index ], next, prev );
      }

   MV_SetReverseStereo( FALSE );

   ASS_PCMSoundDriver = soundcard;

   // Initialize the sound card
   status = SoundDriver_PCM_Init(MixRate, numchannels, samplebits, initdata);
   if ( status != MV_Ok ) {
      MV_SetErrorCode( MV_DriverError );
   }

   if ( MV_ErrorCode != MV_Ok )
      {
      status = MV_ErrorCode;

      free( MV_Voices );
      MV_Voices      = NULL;
      MV_HarshClipTable = NULL;
      MV_TotalMemory = 0;

      MV_SetErrorCode( status );
      return( MV_Error );
      }

   MV_Installed    = TRUE;
   MV_CallBackFunc = NULL;
   MV_RecordFunc   = NULL;
   MV_Recording    = FALSE;
   MV_ReverbLevel  = 0;
   MV_ReverbTable  = NULL;

   // Set the sampling rate
   MV_RequestedMixRate = *MixRate;

   // Set Mixer to play stereo digitized sound
   MV_SetMixMode( *numchannels, *samplebits );
   MV_ReverbDelay = MV_BufferSize * 3;

   // Make sure we don't cross a physical page
   MV_MixBuffer[ MV_NumberOfBuffers ] = ptr;
   for( buffer = 0; buffer < MV_NumberOfBuffers; buffer++ )
      {
      MV_MixBuffer[ buffer ] = ptr;
      ptr += MV_BufferSize;
      }

   // Calculate pan table
   MV_CalcPanTable();

   MV_SetVolume( MV_MaxTotalVolume );

   // Start the playback engine
   status = MV_StartPlayback();
   if ( status != MV_Ok )
      {
      // Preserve error code while we shutdown.
      status = MV_ErrorCode;
      MV_Shutdown();
      MV_SetErrorCode( status );
      return( MV_Error );
      }

#ifdef _XBOX_APU
   // Initialize APU Voice Processor for hardware mixing
   if ( XApuInit() == 0 )
      {
      MV_ApuInitialized = 1;
      MV_ApuActive = 1;
      }
#endif

   return( MV_Ok );
   }


#ifdef _XBOX
/*---------------------------------------------------------------------
   Function: MV_SetSurroundMode

   Enable or disable 5.1 surround sound output.
---------------------------------------------------------------------*/

void MV_SetSurroundMode( int enable )
   {
   MV_SurroundMode = enable ? 1 : 0;
   }

int MV_GetSurroundMode( void )
   {
   return MV_SurroundMode;
   }


/*---------------------------------------------------------------------
   Function: MV_SetVoiceCenter

   Mark or unmark a voice for center channel routing (Duke voice lines).
---------------------------------------------------------------------*/

void MV_SetVoiceCenter( int handle, int center )
   {
   VoiceNode *voice;

   if ( !MV_Installed ) return;

   voice = MV_GetVoice( handle );
   if ( voice == NULL ) return;

   voice->is_center = center ? 1 : 0;

   /* Immediately re-route volumes so first mix buffer uses center.
    * Use 200 as initial level — pan3dsound will correct on next tick. */
   if ( MV_SurroundMode && voice->is_center )
      {
      voice->FLVolume      = &MV_VolumeTable[ 0 ];
      voice->FRVolume      = &MV_VolumeTable[ 0 ];
      voice->CenterVolume  = MV_GetVolumeTable( 200 );
      voice->LFEVolume     = &MV_VolumeTable[ 0 ];
      voice->SLVolume      = &MV_VolumeTable[ 0 ];
      voice->SRVolume      = &MV_VolumeTable[ 0 ];
      }
   }


/*---------------------------------------------------------------------
   Function: MV_SetVoiceSurroundSweep

   Mark a voice to sweep from back-left to back-right over its duration.
   Sets initial volumes to full SL, zero SR.
---------------------------------------------------------------------*/

void MV_SetVoiceSurroundSweep( int handle, int enable )
   {
   VoiceNode *voice;

   if ( !MV_Installed || !MV_SurroundMode ) return;

   voice = MV_GetVoice( handle );
   if ( voice == NULL ) return;

   voice->surround_sweep = enable ? 1 : 0;
   voice->sweep_ticks = 0;
   if ( enable )
      {
      /* Start fully in back-left, silence everywhere else */
      int level = 200;  /* healthy volume level */
      voice->FLVolume      = &MV_VolumeTable[ 0 ];
      voice->FRVolume      = &MV_VolumeTable[ 0 ];
      voice->CenterVolume  = &MV_VolumeTable[ 0 ];
      voice->LFEVolume     = &MV_VolumeTable[ 0 ];
      voice->SLVolume      = MV_GetVolumeTable( level );
      voice->SRVolume      = &MV_VolumeTable[ 0 ];
      }
   }
#endif


/*---------------------------------------------------------------------
   Function: MV_Shutdown

   Restore any resources allocated by Multivoc back to the system.
---------------------------------------------------------------------*/

int MV_Shutdown
   (
   void
   )

   {
   int      buffer;

   if ( !MV_Installed )
      {
      return( MV_Ok );
      }

   MV_KillAllVoices();

#ifdef _XBOX_APU
   if ( MV_ApuInitialized )
      {
      XApuShutdown();
      MV_ApuInitialized = 0;
      MV_ApuActive = 0;
      }
#endif

   MV_Installed = FALSE;

   // Stop the sound recording engine
   if ( MV_Recording )
      {
      MV_StopRecord();
      }

   // Stop the sound playback engine
   MV_StopPlayback();

   // Shutdown the sound card
   SoundDriver_PCM_Shutdown();

   // Free any voices we allocated
   free( MV_Voices );
   MV_Voices      = NULL;
   MV_TotalMemory = 0;

   LL_Reset( (VoiceNode*) &VoiceList, next, prev );
   LL_Reset( (VoiceNode*) &VoicePool, next, prev );

   MV_MaxVoices = 1;

   // Release the descriptor from our mix buffer
   for( buffer = 0; buffer < NumberOfBuffers; buffer++ )
      {
      MV_MixBuffer[ buffer ] = NULL;
      }

   return( MV_Ok );
   }

// vim:ts=3:expandtab:
