/*
 *
 *     GNU Radio IEEE 802.11a/g/n/ac 2x2
 *     Demodulation and decoding of 802.11a/g/n/ac 1x1 and 2x2 formats cuda ver
 *     Copyright (C) Dec 1, 2022  Zelin Yun
 *
 *     This program is free software: you can redistribute it and/or modify
 *     it under the terms of the GNU Affero General Public License as published
 *     by the Free Software Foundation, either version 3 of the License, or
 *     (at your option) any later version.
 *
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU Affero General Public License for more details.
 *
 *     You should have received a copy of the GNU Affero General Public License
 *     along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <gnuradio/io_signature.h>
#include "demodcu2_impl.h"

namespace gr {
  namespace ieee80211cu {

    demodcu2::sptr
    demodcu2::make()
    {
      return gnuradio::make_block_sptr<demodcu2_impl>(
        );
    }


    /*
     * The private constructor
     */
    demodcu2_impl::demodcu2_impl()
      : gr::block("demodcu2",
              gr::io_signature::make(2, 2, sizeof(gr_complex)),
              gr::io_signature::make(0, 0, 0)),
              d_ofdm_fft(64,1)
    {
      message_port_register_out(pmt::mp("out"));
      d_nProc = 0;
      d_nUsed = 0;
      d_debug = true;
      d_sDemod = DEMOD_S_RDTAG;
      d_nPktCorrect = 0;
      memset(d_vhtMcsCount, 0, sizeof(uint64_t) * 10);
      memset(d_legacyMcsCount, 0, sizeof(uint64_t) * 8);
      memset(d_htMcsCount, 0, sizeof(uint64_t) * 8);
      set_tag_propagation_policy(block::TPP_DONT);
      std::cout << "ieee80211 demodcu2, cuda mall2"<<std::endl;
      cuDemodMall2();
      std::cout << "ieee80211 demodcu2, cuda mall2 finish"<<std::endl;
    }

    /*
     * Our virtual destructor.
     */
    demodcu2_impl::~demodcu2_impl()
    {
      std::cout << "ieee80211 demodcu2, cuda free2"<<std::endl;
      cuDemodFree2();
      std::cout << "ieee80211 demodcu2, cuda free2 finish"<<std::endl;
    }

    void
    demodcu2_impl::forecast (int noutput_items, gr_vector_int &ninput_items_required)
    {
      int tmpRequired = noutput_items + 160;
      if(d_sDemod == DEMOD_S_DEMOD)
      {
        if((noutput_items + (d_nSampTotoal - d_nSampCopied)) <= 4096)
          tmpRequired = noutput_items + (d_nSampTotoal - d_nSampCopied);
        else
          tmpRequired = 4096;
      }
      ninput_items_required[0] = tmpRequired;
      ninput_items_required[1] = tmpRequired;
    }

    int
    demodcu2_impl::general_work (int noutput_items,
                       gr_vector_int &ninput_items,
                       gr_vector_const_void_star &input_items,
                       gr_vector_void_star &output_items)
    {
      const gr_complex* inSig = static_cast<const gr_complex*>(input_items[0]);
      const gr_complex* inSig2 = static_cast<const gr_complex*>(input_items[1]);
      d_nProc = std::min(ninput_items[0], ninput_items[1]);
      d_nUsed = 0;

      if(d_sDemod == DEMOD_S_RDTAG)
      {
        // tags, which input, start, end
        get_tags_in_range(tags, 0, nitems_read(0) , nitems_read(0) + 1);
        if (tags.size())
        {
          pmt::pmt_t d_meta = pmt::make_dict();
          for (auto tag : tags){
            d_meta = pmt::dict_add(d_meta, tag.key, tag.value);
          }
          int tmpPktSeq = pmt::to_long(pmt::dict_ref(d_meta, pmt::mp("seq"), pmt::from_long(-1)));
          d_nSigLMcs = pmt::to_long(pmt::dict_ref(d_meta, pmt::mp("mcs"), pmt::from_long(-1)));
          d_nSigLLen = pmt::to_long(pmt::dict_ref(d_meta, pmt::mp("len"), pmt::from_long(-1)));
          d_nSigLSamp = pmt::to_long(pmt::dict_ref(d_meta, pmt::mp("nsamp"), pmt::from_long(-1)));
          std::vector<gr_complex> tmp_csi = pmt::c32vector_elements(pmt::dict_ref(d_meta, pmt::mp("csi"), pmt::PMT_NIL));
          std::copy(tmp_csi.begin(), tmp_csi.end(), d_H);
          dout<<"ieee80211 demodcu2, rd tag seq:"<<tmpPktSeq<<", mcs:"<<d_nSigLMcs<<", len:"<<d_nSigLLen<<", samp:"<<d_nSigLSamp<<std::endl;
          d_nSampConsumed = 0;
          d_nSigLSamp = d_nSigLSamp + 320;
          d_nSampCopied = 0;
          if(d_nSigLMcs > 0)
          {
            signalParserL(d_nSigLMcs, d_nSigLLen, &d_m);
            cuDemodChanSiso((cuFloatComplex*) d_H);
            d_nSampTotoal = d_m.nSymSamp * d_m.nSym;
            d_sDemod = DEMOD_S_DEMOD;
            dout<<"ieee80211 demodcu2, legacy packet"<<std::endl;
          }
          else
          {
            d_sDemod = DEMOD_S_FORMAT;
          }
        }
      }

      if(d_sDemod ==  DEMOD_S_FORMAT && (d_nProc - d_nUsed) >= 160)
      {
        fftDemod(&inSig[d_nUsed + 8], d_fftLtfOut1);
        fftDemod(&inSig[d_nUsed + 8+80], d_fftLtfOut2);
        signalNlDemodDecode(d_fftLtfOut1, d_fftLtfOut2, d_H, d_sigHtIntedLlr, d_sigVhtAIntedLlr);
        //-------------- format check first check vht, then ht otherwise legacy
        procDeintLegacyBpsk(d_sigVhtAIntedLlr, d_sigVhtACodedLlr);
        procDeintLegacyBpsk(&d_sigVhtAIntedLlr[48], &d_sigVhtACodedLlr[48]);
        SV_Decode_Sig(d_sigVhtACodedLlr, d_sigVhtABits, 48);
        if(signalCheckVhtA(d_sigVhtABits))
        {
          // go to vht
          signalParserVhtA(d_sigVhtABits, &d_m, &d_sigVhtA);
          dout<<"ieee80211 demodcu2, vht a check pass nSS:"<<d_m.nSS<<" nLTF:"<<d_m.nLTF<<std::endl;
          d_sDemod = DEMOD_S_VHT;
          d_nSampConsumed += 160;
          d_nUsed += 160;
        }
        else
        {
          procDeintLegacyBpsk(d_sigHtIntedLlr, d_sigHtCodedLlr);
          procDeintLegacyBpsk(&d_sigHtIntedLlr[48], &d_sigHtCodedLlr[48]);
          SV_Decode_Sig(d_sigHtCodedLlr, d_sigHtBits, 48);
          if(signalCheckHt(d_sigHtBits))
          {
            // go to ht
            signalParserHt(d_sigHtBits, &d_m, &d_sigHt);
            dout<<"ieee80211 demodcu2, ht check pass nSS:"<<d_m.nSS<<", nLTF:"<<d_m.nLTF<<", len:"<<d_m.len<<std::endl;
            d_sDemod = DEMOD_S_HT;
            d_nSampConsumed += 160;
            d_nUsed += 160;
          }
          else
          {
            // go to legacy
            signalParserL(d_nSigLMcs, d_nSigLLen, &d_m);
            cuDemodChanSiso((cuFloatComplex*) d_H);
            d_nSampTotoal = d_m.nSymSamp * d_m.nSym;
            d_sDemod = DEMOD_S_DEMOD;
            dout<<"ieee80211 demodcu2, check format legacy packet"<<std::endl;
          }
        }
      }

      if(d_sDemod == DEMOD_S_VHT && ((d_nProc - d_nUsed) >= (80 + d_m.nLTF*80 + 80)))
      {
        nonLegacyChanEstimate(&inSig[d_nUsed + 80], &inSig2[d_nUsed + 80]);
        vhtSigBDemod(&inSig[d_nUsed + 80 + d_m.nLTF*80], &inSig2[d_nUsed + 80 + d_m.nLTF*80]);
        signalParserVhtB(d_sigVhtB20Bits, &d_m);
        dout<<"ieee80211 demodcu2, vht b len:"<<d_m.len<<", mcs:"<<d_m.mcs<<", nSS:"<<d_m.nSS<<", nSym:"<<d_m.nSym<<std::endl;
        int tmpNLegacySym = (d_nSigLLen*8 + 22 + 23)/24;
        if(d_m.len > 0 && d_m.len <= 4095 && d_m.nSS <= 2 && (tmpNLegacySym * 80) >= (d_m.nSym * d_m.nSymSamp + 160 + 80 + d_m.nLTF * 80 + 80))
        {
          if(d_m.nSS == 1)
          {
            cuDemodChanSiso((cuFloatComplex*) d_H_NL);
          }
          else
          {
            cuDemodChanMimo((cuFloatComplex*) d_H_NL22, (cuFloatComplex*) d_H_NL22_INV, (cuFloatComplex*) d_pilotNlLtf);
          }
          d_nSampTotoal = d_m.nSymSamp * d_m.nSym;
          d_sDemod = DEMOD_S_DEMOD;
          d_nSampConsumed += (80 + d_m.nLTF*80 + 80);
          d_nUsed += (80 + d_m.nLTF*80 + 80);
        }
        else
        {
          d_sDemod = DEMOD_S_CLEAN;
        }
      }

      if(d_sDemod == DEMOD_S_HT && ((d_nProc - d_nUsed) >= (80 + d_m.nLTF*80)))
      {
        nonLegacyChanEstimate(&inSig[d_nUsed + 80], &inSig2[d_nUsed + 80]);
        int tmpNLegacySym = (d_nSigLLen*8 + 22 + 23)/24;
        if(d_m.len > 0 && d_m.len <= 4095 && d_m.nSS <= 2 && (tmpNLegacySym * 80) >= (d_m.nSym * d_m.nSymSamp + 160 + 80 + d_m.nLTF * 80))
        {
          if(d_m.nSS == 1)
          {
            cuDemodChanSiso((cuFloatComplex*) d_H_NL);
          }
          else
          {
            cuDemodChanMimo((cuFloatComplex*) d_H_NL22, (cuFloatComplex*) d_H_NL22_INV, (cuFloatComplex*) d_pilotNlLtf);
          }
          d_nSampTotoal = d_m.nSymSamp * d_m.nSym;
          d_sDemod = DEMOD_S_DEMOD;
          d_nSampConsumed += (80 + d_m.nLTF*80);
          d_nUsed += (80 + d_m.nLTF*80);
        }
        else
        {
          d_sDemod = DEMOD_S_CLEAN;
        }
      }

      if(d_sDemod == DEMOD_S_DEMOD)
      {
        // copy samples to GPU
        if((d_nProc - d_nUsed) >= (d_nSampTotoal - d_nSampCopied))
        {
          // copy and decode
          if(d_m.nSS == 1)
          {
            cuDemodSigCopy(d_nSampCopied, (d_nSampTotoal - d_nSampCopied), (const cuFloatComplex*) &inSig[d_nUsed]);
            cuDemodSiso(&d_m, d_psduBytes);
          }
          else
          {
            cuDemodSigCopy2(d_nSampCopied, d_nSampCopied + d_m.nSym * 80, (d_nSampTotoal - d_nSampCopied), (const cuFloatComplex*) &inSig[d_nUsed], (const cuFloatComplex*) &inSig2[d_nUsed]);
            cuDemodMimo(&d_m, d_psduBytes);
          }
          d_nSampConsumed += (d_nSampTotoal - d_nSampCopied);
          d_nUsed += (d_nSampTotoal - d_nSampCopied);
          packetAssemble();
          d_sDemod = DEMOD_S_CLEAN;
        }
        else
        {
          // copy
          if(d_m.nSS == 1)
          {
            cuDemodSigCopy(d_nSampCopied, (d_nProc - d_nUsed), (const cuFloatComplex*) &inSig[d_nUsed]);
          }
          else
          {
            cuDemodSigCopy2(d_nSampCopied, d_nSampCopied + d_m.nSym * 80, (d_nProc - d_nUsed), (const cuFloatComplex*) &inSig[d_nUsed], (const cuFloatComplex*) &inSig2[d_nUsed]);
          }
          d_nSampCopied += (d_nProc - d_nUsed);
          d_nSampConsumed += (d_nProc - d_nUsed);
          d_nUsed = d_nProc;
        }
      }

      if(d_sDemod == DEMOD_S_CLEAN)
      {
        if((d_nProc - d_nUsed) >= (d_nSigLSamp - d_nSampConsumed))
        {
          d_nUsed += (d_nSigLSamp - d_nSampConsumed);
          d_sDemod = DEMOD_S_RDTAG;
        }
        else
        {
          d_nSampConsumed += (d_nProc - d_nUsed);
          d_nUsed = d_nProc;
        }
      }

      consume_each (d_nUsed);
      return 0;
    }

    void
    demodcu2_impl::fftDemod(const gr_complex* sig, gr_complex* res)
    {
      memcpy(d_ofdm_fft.get_inbuf(), sig, sizeof(gr_complex)*64);
      d_ofdm_fft.execute();
      memcpy(res, d_ofdm_fft.get_outbuf(), sizeof(gr_complex)*64);
    }

    void
    demodcu2_impl::nonLegacyChanEstimate(const gr_complex* sig1, const gr_complex* sig2)
    {
      // only supports SISO and SU-MIMO 2x2
      // MU-MIMO and channel esti are to be added
      if(d_m.nSS == 1)
      {
        if(d_m.nLTF == 1)
        {
          fftDemod(&sig1[8], d_fftLtfOut1);
          for(int i=0;i<64;i++)
          {
            if(i==0 || (i>=29 && i<=35))
            {}
            else
            {
              d_H_NL[i] = d_fftLtfOut1[i] / LTF_NL_28_F_FLOAT[i];
            }
          }
        }
      }
      else if(d_m.nSS == 2)
      {
        fftDemod(&sig1[8], d_fftLtfOut1);
        fftDemod(&sig2[8], d_fftLtfOut2);
        fftDemod(&sig1[8+80], d_fftLtfOut12);
        fftDemod(&sig2[8+80], d_fftLtfOut22);

        for(int i=0;i<64;i++)
        {
          if(i==0 || (i>=29 && i<=35))
          {
          }
          else
          {
            d_H_NL22[0][i] = (d_fftLtfOut1[i] - d_fftLtfOut12[i])*LTF_NL_28_F_FLOAT2[i];
            d_H_NL22[1][i] = (d_fftLtfOut2[i] - d_fftLtfOut22[i])*LTF_NL_28_F_FLOAT2[i];
            d_H_NL22[2][i] = (d_fftLtfOut1[i] + d_fftLtfOut12[i])*LTF_NL_28_F_FLOAT2[i];
            d_H_NL22[3][i] = (d_fftLtfOut2[i] + d_fftLtfOut22[i])*LTF_NL_28_F_FLOAT2[i];
          }
        }
        if(d_m.format == C8P_F_VHT)
        {
          d_H_NL22[0][7] = (d_H_NL22[0][6] + d_H_NL22[0][8]) / 2.0f;
          d_H_NL22[1][7] = (d_H_NL22[1][6] + d_H_NL22[1][8]) / 2.0f;
          d_H_NL22[2][7] = (d_H_NL22[2][6] + d_H_NL22[2][8]) / 2.0f;
          d_H_NL22[3][7] = (d_H_NL22[3][6] + d_H_NL22[3][8]) / 2.0f;
          d_H_NL22[0][21] = (d_H_NL22[0][20] + d_H_NL22[0][22]) / 2.0f;
          d_H_NL22[1][21] = (d_H_NL22[1][20] + d_H_NL22[1][22]) / 2.0f;
          d_H_NL22[2][21] = (d_H_NL22[2][20] + d_H_NL22[2][22]) / 2.0f;
          d_H_NL22[3][21] = (d_H_NL22[3][20] + d_H_NL22[3][22]) / 2.0f;
          d_H_NL22[0][43] = (d_H_NL22[0][42] + d_H_NL22[0][44]) / 2.0f;
          d_H_NL22[1][43] = (d_H_NL22[1][42] + d_H_NL22[1][44]) / 2.0f;
          d_H_NL22[2][43] = (d_H_NL22[2][42] + d_H_NL22[2][44]) / 2.0f;
          d_H_NL22[3][43] = (d_H_NL22[3][42] + d_H_NL22[3][44]) / 2.0f;
          d_H_NL22[0][57] = (d_H_NL22[0][56] + d_H_NL22[0][58]) / 2.0f;
          d_H_NL22[1][57] = (d_H_NL22[1][56] + d_H_NL22[1][58]) / 2.0f;
          d_H_NL22[2][57] = (d_H_NL22[2][56] + d_H_NL22[2][58]) / 2.0f;
          d_H_NL22[3][57] = (d_H_NL22[3][56] + d_H_NL22[3][58]) / 2.0f;
        }
        gr_complex tmpadbc, a, b, c, d;
        for(int i=0;i<64;i++)
        {
          if(i==0 || (i>=29 && i<=35))
          {
          }
          else
          {
            a = d_H_NL22[0][i] * std::conj(d_H_NL22[0][i]) + d_H_NL22[1][i] * std::conj(d_H_NL22[1][i]);
            b = d_H_NL22[0][i] * std::conj(d_H_NL22[2][i]) + d_H_NL22[1][i] * std::conj(d_H_NL22[3][i]);
            c = d_H_NL22[2][i] * std::conj(d_H_NL22[0][i]) + d_H_NL22[3][i] * std::conj(d_H_NL22[1][i]);
            d = d_H_NL22[2][i] * std::conj(d_H_NL22[2][i]) + d_H_NL22[3][i] * std::conj(d_H_NL22[3][i]);
            tmpadbc = 1.0f/(a*d - b*c);

            d_H_NL22_INV[0][i] = tmpadbc*d;
            d_H_NL22_INV[1][i] = -tmpadbc*b;
            d_H_NL22_INV[2][i] = -tmpadbc*c;
            d_H_NL22_INV[3][i] = tmpadbc*a;
          }
        }
        // get the pilots from nl ltf
        // only for 2x2
        gr_complex tmp1, tmp2, tmps1, tmps2;
        int i = 7;
        tmp1 = d_fftLtfOut1[i] * std::conj(d_H_NL22[0][i]) + d_fftLtfOut2[i] * std::conj(d_H_NL22[1][i]);
        tmp2 = d_fftLtfOut1[i] * std::conj(d_H_NL22[2][i]) + d_fftLtfOut2[i] * std::conj(d_H_NL22[3][i]);
        tmps1 = tmp1 * d_H_NL22_INV[0][i] + tmp2 * d_H_NL22_INV[2][i];
        tmps2 = tmp1 * d_H_NL22_INV[1][i] + tmp2 * d_H_NL22_INV[3][i];
        d_pilotNlLtf[2] = std::conj(tmps1);
        d_pilotNlLtf[6] = std::conj(tmps2);

        i = 21;
        tmp1 = d_fftLtfOut1[i] * std::conj(d_H_NL22[0][i]) + d_fftLtfOut2[i] * std::conj(d_H_NL22[1][i]);
        tmp2 = d_fftLtfOut1[i] * std::conj(d_H_NL22[2][i]) + d_fftLtfOut2[i] * std::conj(d_H_NL22[3][i]);
        tmps1 = tmp1 * d_H_NL22_INV[0][i] + tmp2 * d_H_NL22_INV[2][i];
        tmps2 = tmp1 * d_H_NL22_INV[1][i] + tmp2 * d_H_NL22_INV[3][i];
        d_pilotNlLtf[3] = std::conj(tmps1);
        d_pilotNlLtf[7] = std::conj(tmps2);

        i = 43;
        tmp1 = d_fftLtfOut1[i] * std::conj(d_H_NL22[0][i]) + d_fftLtfOut2[i] * std::conj(d_H_NL22[1][i]);
        tmp2 = d_fftLtfOut1[i] * std::conj(d_H_NL22[2][i]) + d_fftLtfOut2[i] * std::conj(d_H_NL22[3][i]);
        tmps1 = tmp1 * d_H_NL22_INV[0][i] + tmp2 * d_H_NL22_INV[2][i];
        tmps2 = tmp1 * d_H_NL22_INV[1][i] + tmp2 * d_H_NL22_INV[3][i];
        d_pilotNlLtf[0] = std::conj(tmps1);
        d_pilotNlLtf[4] = std::conj(tmps2);

        i = 57;
        tmp1 = d_fftLtfOut1[i] * std::conj(d_H_NL22[0][i]) + d_fftLtfOut2[i] * std::conj(d_H_NL22[1][i]);
        tmp2 = d_fftLtfOut1[i] * std::conj(d_H_NL22[2][i]) + d_fftLtfOut2[i] * std::conj(d_H_NL22[3][i]);
        tmps1 = tmp1 * d_H_NL22_INV[0][i] + tmp2 * d_H_NL22_INV[2][i];
        tmps2 = tmp1 * d_H_NL22_INV[1][i] + tmp2 * d_H_NL22_INV[3][i];
        d_pilotNlLtf[1] = std::conj(-tmps1);
        d_pilotNlLtf[5] = std::conj(-tmps2);
      }
      else
      {
        // not supported
      }
    }

    void
    demodcu2_impl::vhtSigBDemod(const gr_complex* sig1, const gr_complex* sig2)
    {
      if(d_m.nSS == 1)
      {
        fftDemod(&sig1[8], d_fftLtfOut1);
        for(int i=0;i<64;i++)
        {
          if(i==0 || (i>=29 && i<=35))
          {}
          else
          {
            d_sig1[i] = d_fftLtfOut1[i] / d_H_NL[i];
          }
        }
        gr_complex tmpPilotSum = std::conj(d_sig1[7] - d_sig1[21] + d_sig1[43] + d_sig1[57]);
        float tmpPilotSumAbs = std::abs(tmpPilotSum);
        int j=26;
        for(int i=0;i<64;i++)
        {
          if(i==0 || (i>=29 && i<=35) || i==7 || i==21 || i==43 || i==57)
          {}
          else
          {
            d_sigVhtB20IntedLlr[j] = (d_sig1[i] * tmpPilotSum / tmpPilotSumAbs).real();
            j++;
            if(j >= 52){j = 0;}
          }
        }
      }
      else if(d_m.nSS == 2)
      {
        fftDemod(&sig1[8], d_fftLtfOut1);
        fftDemod(&sig2[8], d_fftLtfOut2);
        for(int i=0;i<64;i++)
        {
          if(i==0 || (i>=29 && i<=35))
          {}
          else
          {
            gr_complex tmp1 = d_fftLtfOut1[i] * std::conj(d_H_NL22[0][i]) + d_fftLtfOut2[i] * std::conj(d_H_NL22[1][i]);
            gr_complex tmp2 = d_fftLtfOut1[i] * std::conj(d_H_NL22[2][i]) + d_fftLtfOut2[i] * std::conj(d_H_NL22[3][i]);
            d_sig1[i] = tmp1 * d_H_NL22_INV[0][i] + tmp2 * d_H_NL22_INV[2][i];
            d_sig2[i] = tmp1 * d_H_NL22_INV[1][i] + tmp2 * d_H_NL22_INV[3][i];
          }
        }
        gr_complex tmpPilotSum = std::conj(
          d_sig1[7]*d_pilotNlLtf[2] - 
          d_sig1[21]*d_pilotNlLtf[3] + 
          d_sig1[43]*d_pilotNlLtf[0] + 
          d_sig1[57]*d_pilotNlLtf[1] +
          d_sig2[7]*d_pilotNlLtf[6] - 
          d_sig2[21]*d_pilotNlLtf[7] + 
          d_sig2[43]*d_pilotNlLtf[4] + 
          d_sig2[57]*d_pilotNlLtf[5]);
        float tmpPilotSumAbs = std::abs(tmpPilotSum);
        int j=26;
        for(int i=0;i<64;i++)
        {
          if(i==0 || (i>=29 && i<=35) || i==7 || i==21 || i==43 || i==57)
          {}
          else
          {
            d_sigVhtB20IntedLlr[j] = ((d_sig1[i] * tmpPilotSum / tmpPilotSumAbs).real() + (d_sig2[i] * tmpPilotSum / tmpPilotSumAbs).real())/2.0f;
            j++;
            if(j >= 52){j = 0;}
          }
        }
      }
      else
      {
        memset(d_sigVhtB20Bits, 0, 26);
        return;
      }
   
      for(int i=0;i<52;i++)
      {
        d_sigVhtB20CodedLlr[mapDeintVhtSigB20[i]] = d_sigVhtB20IntedLlr[i];
      }
      SV_Decode_Sig(d_sigVhtB20CodedLlr, d_sigVhtB20Bits, 26);
    }

  } /* namespace ieee80211cu */
} /* namespace gr */
