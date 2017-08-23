﻿// -----------------------------------------------------------------------------------------
// NVEnc by rigaya
// -----------------------------------------------------------------------------------------
//
// The MIT License
//
// Copyright (c) 2014-2016 rigaya
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// ------------------------------------------------------------------------------------------

#include <map>
#include <array>
#include "convert_csp.h"
#include "NVEncFilterAfs.h"
#include "NVEncParam.h"
#include "afs_stg.h"
#pragma warning (push)

static void afs_get_motion_count_simd(int *motion_count, const uint8_t *ptr, const AFS_SCAN_CLIP *clip, int pitch, int scan_w, int scan_h, int tb_order);
static void afs_get_stripe_count_simd(int *stripe_count, const uint8_t *ptr, const AFS_SCAN_CLIP *clip, int pitch, int scan_w, int scan_h, int tb_order);

template<typename T>
T max3(T a, T b, T c) {
    return std::max(std::max(a, b), c);
}
template<typename T>
T absdiff(T a, T b) {
    T a_b = a - b;
    T b_a = b - a;
    return (a >= b) ? a_b : b_a;
}

afsSourceCache::afsSourceCache() :
    m_sourceArray(),
    m_nFramesInput(0) {
}

cudaError_t afsSourceCache::alloc(const FrameInfo& frameInfo) {
    for (int i = 0; i < _countof(m_sourceArray); i++) {
        m_sourceArray[i].frame = frameInfo;
        auto ret = m_sourceArray[i].alloc();
        if (ret != cudaSuccess) {
            m_sourceArray[i].clear();
            return ret;
        }
    }
    return cudaSuccess;
}

cudaError_t afsSourceCache::add(const FrameInfo *pInputFrame, cudaStream_t stream) {
    const int iframe = m_nFramesInput++;
    auto pDstFrame = get(iframe);
    pDstFrame->frame.flags     = pInputFrame->flags;
    pDstFrame->frame.picstruct = pInputFrame->picstruct;
    pDstFrame->frame.timestamp = pInputFrame->timestamp;
    pDstFrame->frame.duration  = pInputFrame->duration;

    const auto frameOutInfoEx = getFrameInfoExtra(pInputFrame);
    static const auto supportedCspYV12   = make_array<RGY_CSP>(RGY_CSP_YV12, RGY_CSP_YV12_09, RGY_CSP_YV12_10, RGY_CSP_YV12_12, RGY_CSP_YV12_14, RGY_CSP_YV12_16);
    static const auto supportedCspYUV444 = make_array<RGY_CSP>(RGY_CSP_YUV444, RGY_CSP_YUV444_09, RGY_CSP_YUV444_10, RGY_CSP_YUV444_12, RGY_CSP_YUV444_14, RGY_CSP_YUV444_16);
    auto cudaerr = cudaSuccess;
    if (std::find(supportedCspYV12.begin(), supportedCspYV12.end(), pInputFrame->csp) != supportedCspYV12.end()) {
        //Y
        cudaerr = cudaMemcpy2DAsync((uint8_t *)pDstFrame->frame.ptr, pDstFrame->frame.pitch,
            (uint8_t *)pInputFrame->ptr, pInputFrame->pitch,
            frameOutInfoEx.width_byte, pDstFrame->frame.height, cudaMemcpyDeviceToDevice, stream);
        if (cudaerr != cudaSuccess) return cudaerr;

        //Uフィールド分離
        cudaerr = cudaMemcpy2DAsync((uint8_t *)pDstFrame->frame.ptr + pDstFrame->frame.pitch * pDstFrame->frame.height,
            pDstFrame->frame.pitch,
            (uint8_t *)pInputFrame->ptr + (pInputFrame->height + 0) * pInputFrame->pitch,
            pInputFrame->pitch * 2, //偶数ラインのみ取り出し
            frameOutInfoEx.width_byte >> 1, pDstFrame->frame.height >> 2, cudaMemcpyDeviceToDevice, stream);
        if (cudaerr != cudaSuccess) return cudaerr;

        cudaerr = cudaMemcpy2DAsync((uint8_t *)pDstFrame->frame.ptr + pDstFrame->frame.pitch * pDstFrame->frame.height * 5 / 4,
            pDstFrame->frame.pitch,
            (uint8_t *)pInputFrame->ptr + (pInputFrame->height + 1) * pInputFrame->pitch,
            pInputFrame->pitch * 2, //奇数ラインのみ取り出し
            frameOutInfoEx.width_byte >> 1, pDstFrame->frame.height >> 2, cudaMemcpyDeviceToDevice, stream);
        if (cudaerr != cudaSuccess) return cudaerr;

        //Vフィールド分離
        cudaerr = cudaMemcpy2DAsync((uint8_t *)pDstFrame->frame.ptr + pDstFrame->frame.pitch * pDstFrame->frame.height * 6 / 4,
            pDstFrame->frame.pitch,
            (uint8_t *)pInputFrame->ptr + (pInputFrame->height * 3 / 2 + 0) * pInputFrame->pitch,
            pInputFrame->pitch * 2, //偶数ラインのみ取り出し
            frameOutInfoEx.width_byte >> 1, pDstFrame->frame.height >> 2, cudaMemcpyDeviceToDevice, stream);
        if (cudaerr != cudaSuccess) return cudaerr;

        cudaerr = cudaMemcpy2DAsync((uint8_t *)pDstFrame->frame.ptr + pDstFrame->frame.pitch * pDstFrame->frame.height * 7 / 4,
            pDstFrame->frame.pitch,
            (uint8_t *)pInputFrame->ptr + (pInputFrame->height * 3 / 2 + 1) * pInputFrame->pitch,
            pInputFrame->pitch * 2, //奇数ラインのみ取り出し
            frameOutInfoEx.width_byte >> 1, pDstFrame->frame.height >> 2, cudaMemcpyDeviceToDevice, stream);
        if (cudaerr != cudaSuccess) return cudaerr;

    } else if (std::find(supportedCspYUV444.begin(), supportedCspYUV444.end(), pInputFrame->csp) != supportedCspYUV444.end()) {
        cudaerr = cudaMemcpy2DAsync((uint8_t *)pDstFrame->frame.ptr, pDstFrame->frame.pitch,
            (uint8_t *)pInputFrame->ptr, pInputFrame->pitch,
            frameOutInfoEx.width_byte, frameOutInfoEx.height_total, cudaMemcpyDeviceToDevice, stream);
    } else {
        cudaerr = cudaErrorNotSupported;
    }
    return cudaerr;
}

void afsSourceCache::clear() {
    for (int i = 0; i < _countof(m_sourceArray); i++) {
        m_sourceArray[i].clear();
    }
    m_nFramesInput = 0;
}

afsSourceCache::~afsSourceCache() {
    clear();
}

afsScanCache::afsScanCache() :
    m_scanArray() {
}

void afsScanCache::initcache(int iframe) {
    auto data = get(iframe);
    data->status = 0;
    data->frame = 0;
    data->tb_order = 0;
    data->thre_shift = 0;
    data->thre_deint = 0;
    data->thre_Ymotion = 0;
    data->thre_Cmotion = 0;
    memset(&data->clip, 0, sizeof(data->clip));
    data->ff_motion = 0;
    data->lf_motion = 0;
}

cudaError_t afsScanCache::alloc(const FrameInfo& frameInfo) {
    for (int i = 0; i < _countof(m_scanArray); i++) {
        initcache(i);
        m_scanArray[i].map.frame = frameInfo;
        m_scanArray[i].map.frame.csp = RGY_CSP_NV12;
        auto ret = m_scanArray[i].map.alloc();
        if (ret != cudaSuccess) {
            m_scanArray[i].map.clear();
            return ret;
        }
        m_scanArray[i].status = 0;
    }
    return cudaSuccess;
}

void afsScanCache::clear() {
    for (int i = 0; i < _countof(m_scanArray); i++) {
        m_scanArray[i].map.clear();
        initcache(i);
    }
}

afsScanCache::~afsScanCache() {
    clear();
}

afsStripeCache::afsStripeCache() :
    m_stripeArray() {
}

void afsStripeCache::initcache(int iframe) {
    auto data = get(iframe);
    data->status = 0;
    data->frame = 0;
    data->count0 = 0;
    data->count1 = 0;
}

void afsStripeCache::expire(int iframe) {
    auto stp = get(iframe);
    if (stp->frame == iframe && stp->status > 0) {
        stp->status = 0;
    }
}

cudaError_t afsStripeCache::alloc(const FrameInfo& frameInfo) {
    for (int i = 0; i < _countof(m_stripeArray); i++) {
        initcache(i);
        m_stripeArray[i].map.frame = frameInfo;
        m_stripeArray[i].map.frame.csp = RGY_CSP_NV12;
        auto ret = m_stripeArray[i].map.alloc();
        if (ret != cudaSuccess) {
            m_stripeArray[i].map.clear();
            return ret;
        }
    }
    return cudaSuccess;
}

AFS_STRIPE_DATA *afsStripeCache::filter(int iframe, int analyze, cudaStream_t stream, cudaError_t *pErr) {
    auto sip = get(iframe);
    if (analyze > 1) {
        auto sip_dst = getFiltered();
        if (cudaSuccess != (*pErr = map_filter(sip_dst, sip, stream))) {
            sip_dst = nullptr;
        }
        sip = sip_dst;
    }
    return sip;
}

void afsStripeCache::clear() {
    for (int i = 0; i < _countof(m_stripeArray); i++) {
        m_stripeArray[i].map.clear();
        initcache(i);
    }
}

afsStripeCache::~afsStripeCache() {
    clear();
}

afsStreamStatus::afsStreamStatus() :
    m_initialized(false),
    m_quarter_jitter(0),
    m_additional_jitter(0),
    m_phase24(0),
    m_position24(0),
    m_prev_jitter(0),
    m_prev_rff_smooth(0),
    m_prev_status(0),
    m_set_frame(-1),
    m_pos(),
    m_fpLog() {
};

afsStreamStatus::~afsStreamStatus() {
    m_fpLog.reset();
}

void afsStreamStatus::init(uint8_t status, int drop24) {
    m_prev_status = status;
    m_prev_jitter = 0;
    m_additional_jitter = 0;
    m_prev_rff_smooth = 0;
    m_phase24 = 4;
    m_position24 = 0;
    if (drop24 ||
        (!(status & AFS_FLAG_SHIFT0) &&
        (status & AFS_FLAG_SHIFT1) &&
            (status & AFS_FLAG_SHIFT2))) {
        m_phase24 = 0;
    }
    if (status & AFS_FLAG_FORCE24) {
        m_position24++;
    } else {
        m_phase24 -= m_position24 + 1;
        m_position24 = 0;
    }
    m_initialized = true;
}

int afsStreamStatus::open_log(const tstring& log_filename) {
    FILE *fp = NULL;
    if (_tfopen_s(&fp, log_filename.c_str(), _T("w"))) {
        return 1;
    }
    m_fpLog = unique_ptr<FILE, fp_deleter>(fp, fp_deleter());
    fprintf(m_fpLog.get(), " iframe,  sts,       ,        pos,   orig_pts, q_jit, prevjit, pos24, phase24, rff_smooth\n");
    return 0;
}

void afsStreamStatus::write_log(const afsFrameTs *const frameTs) {
    if (!m_fpLog) {
        return;
    }
    fprintf(m_fpLog.get(), "%7d, 0x%2x, %s%s%s%s%s%s, %10lld, %10lld, %3d, %3d, %3d, %3d, %3d\n",
        frameTs->iframe,
        m_prev_status,
        m_prev_status & AFS_FLAG_PROGRESSIVE ? "p" : "i",
        ISRFF(m_prev_status) ? "r" : "-",
        (((m_prev_status & AFS_FLAG_PROGRESSIVE) ? 0 : m_prev_status) & AFS_FLAG_SHIFT0) ? "0" : "-",
        (((m_prev_status & AFS_FLAG_PROGRESSIVE) ? 0 : m_prev_status) & AFS_FLAG_SHIFT1) ? "1" : "-",
        (((m_prev_status & AFS_FLAG_PROGRESSIVE) ? 0 : m_prev_status) & AFS_FLAG_SHIFT2) ? "2" : "-",
        (((m_prev_status & AFS_FLAG_PROGRESSIVE) ? 0 : m_prev_status) & AFS_FLAG_SHIFT3) ? "3" : "-",
        frameTs->pos, frameTs->orig_pts,
        m_quarter_jitter, m_prev_jitter, m_position24, m_phase24, m_prev_rff_smooth);
    return;
}

int afsStreamStatus::set_status(int iframe, uint8_t status, int drop24, int64_t orig_pts) {
    afsFrameTs *const frameTs = &m_pos[iframe & 15];
    frameTs->iframe = iframe;
    frameTs->orig_pts = orig_pts;
    if (!m_initialized) {
        init(status, 0);
        frameTs->pos = orig_pts;
        m_set_frame = iframe;
        write_log(frameTs);
        return 0;
    }
    if (iframe > m_set_frame + 1) {
        return 1;
    }
    m_set_frame = iframe;

    int pull_drop = 0;
    int quarter_jitter = 0;
    int rff_smooth = 0;
    if (status & AFS_FLAG_PROGRESSIVE) {
        if (status & (AFS_FLAG_FORCE24 | AFS_FLAG_SMOOTHING)) {
            if (!m_prev_rff_smooth) {
                if (ISRFF(m_prev_status)) rff_smooth = -1;
                else if ((m_prev_status & AFS_FLAG_PROGRESSIVE) && ISRFF(status)) rff_smooth = 1;
            }
            quarter_jitter = rff_smooth;
        }
        pull_drop = 0;
        m_additional_jitter = 0;
        drop24 = 0;
    } else {
        if (status & AFS_FLAG_SHIFT0) {
            quarter_jitter = -2;
        } else if (m_prev_status & AFS_FLAG_SHIFT0) {
            quarter_jitter = (status & AFS_FLAG_SMOOTHING) ? -1 : -2;
        } else {
            quarter_jitter = 0;
        }
        quarter_jitter += ((status & AFS_FLAG_SMOOTHING) || m_additional_jitter != -1) ? m_additional_jitter : -2;

        if (status & (AFS_FLAG_FORCE24 | AFS_FLAG_SMOOTHING)) {
            if (!m_prev_rff_smooth) {
                if (ISRFF(m_prev_status)) rff_smooth = -1;
                else if ((m_prev_status & AFS_FLAG_PROGRESSIVE) && ISRFF(status)) rff_smooth = 1;
            }
        }
        quarter_jitter += rff_smooth;
        m_position24 += rff_smooth;

        pull_drop = (status & AFS_FLAG_FRAME_DROP)
            && !((m_prev_status|status) & AFS_FLAG_SHIFT0)
            && (status & AFS_FLAG_SHIFT1);
        m_additional_jitter = pull_drop ? -1 : 0;

        drop24 = drop24 ||
            (!(status & AFS_FLAG_SHIFT0) &&
              (status & AFS_FLAG_SHIFT1) &&
              (status & AFS_FLAG_SHIFT2));
    }

    if (drop24) m_phase24 = (m_position24 + 100) % 5;
    drop24 = 0;
    if (m_position24 >= m_phase24 &&
        ((m_position24 + 100) % 5 == m_phase24 ||
         (m_position24 +  99) % 5 == m_phase24)) {
        m_position24 -= 5;
        drop24 = 1;
    }

    if (status & AFS_FLAG_FORCE24) {
        pull_drop = drop24;
        if (status & AFS_FLAG_PROGRESSIVE) {
            quarter_jitter += m_position24;
        } else {
            quarter_jitter = m_position24++;
        }
    } else if (!(status & AFS_FLAG_PROGRESSIVE)) {
        m_phase24 -= m_position24 + 1;
        m_position24 = 0;
    }
    int drop_thre = (status & AFS_FLAG_FRAME_DROP) ? 0 : -3;
    if (!(status & AFS_FLAG_PROGRESSIVE) && ISRFF(m_prev_status)) {
        //rffからの切替時はなるべくdropさせない
        drop_thre = -3;
    }
    int drop = (quarter_jitter - m_prev_jitter < drop_thre);

    m_quarter_jitter = quarter_jitter;
    m_prev_rff_smooth = rff_smooth;
    m_prev_status = status;

    drop |= pull_drop;
    if (drop) {
        m_prev_jitter -= 4;
        m_quarter_jitter = 0;
        frameTs->pos = AFS_SSTS_DROP; //drop
    } else {
        m_prev_jitter = m_quarter_jitter;
        frameTs->pos = frameTs->orig_pts + m_quarter_jitter;
    }
    write_log(frameTs);
    return 0;
}

int64_t afsStreamStatus::get_duration(int64_t iframe) {
    if (m_set_frame < iframe + 2) {
        return AFS_SSTS_ERROR;
    }
    auto iframe_pos = m_pos[(iframe + 0) & 15].pos;
    if (iframe_pos < 0) {
        return AFS_SSTS_DROP;
    }
    auto next_pos = m_pos[(iframe + 1) & 15].pos;
    if (next_pos < 0) {
        //iframe + 1がdropならその先のフレームを参照
        next_pos = m_pos[(iframe + 2) & 15].pos;
    }
    if (next_pos < 0) {
        //iframe + 1がdropならその先のフレームを参照
        next_pos = m_pos[(iframe + 3) & 15].pos;
    }
    return next_pos - iframe_pos;
}

NVEncFilterAfs::NVEncFilterAfs() :
    m_stream(),
    m_nFrame(0),
    m_nPts(0),
    m_source(),
    m_scan(),
    m_stripe(),
    m_status(),
    m_streamsts(),
    m_count_motion(),
    m_count_stripe(),
    m_fpTimecode() {
    m_sFilterName = _T("afs");
}

NVEncFilterAfs::~NVEncFilterAfs() {
    close();
}

void NVEncFilterAfs::set_preset(VppAfs *pVppAfs, int preset) {
    switch (preset) {
    case AFS_PRESET_DEFAULT: //デフォルト
        pVppAfs->method_switch = FILTER_DEFAULT_AFS_METHOD_SWITCH;
        pVppAfs->coeff_shift   = FILTER_DEFAULT_AFS_COEFF_SHIFT;
        pVppAfs->thre_shift    = FILTER_DEFAULT_AFS_THRE_SHIFT;
        pVppAfs->thre_deint    = FILTER_DEFAULT_AFS_THRE_DEINT;
        pVppAfs->thre_Ymotion  = FILTER_DEFAULT_AFS_THRE_YMOTION;
        pVppAfs->thre_Cmotion  = FILTER_DEFAULT_AFS_THRE_CMOTION;
        pVppAfs->analyze       = FILTER_DEFAULT_AFS_ANALYZE;
        pVppAfs->shift         = FILTER_DEFAULT_AFS_SHIFT;
        pVppAfs->drop          = FILTER_DEFAULT_AFS_DROP;
        pVppAfs->smooth        = FILTER_DEFAULT_AFS_SMOOTH;
        pVppAfs->force24       = FILTER_DEFAULT_AFS_FORCE24;
        pVppAfs->tune          = FILTER_DEFAULT_AFS_TUNE;
        break;
    case AFS_PRESET_TRIPLE: //動き重視
        pVppAfs->method_switch = 0;
        pVppAfs->coeff_shift   = 192;
        pVppAfs->thre_shift    = 128;
        pVppAfs->thre_deint    = 48;
        pVppAfs->thre_Ymotion  = 112;
        pVppAfs->thre_Cmotion  = 224;
        pVppAfs->analyze       = 1;
        pVppAfs->shift         = false;
        pVppAfs->drop          = false;
        pVppAfs->smooth        = false;
        pVppAfs->force24       = false;
        pVppAfs->tune          = false;
        break;
    case AFS_PRESET_DOUBLE://二重化
        pVppAfs->method_switch = 0;
        pVppAfs->coeff_shift   = 192;
        pVppAfs->thre_shift    = 128;
        pVppAfs->thre_deint    = 48;
        pVppAfs->thre_Ymotion  = 112;
        pVppAfs->thre_Cmotion  = 224;
        pVppAfs->analyze       = 2;
        pVppAfs->shift         = true;
        pVppAfs->drop          = true;
        pVppAfs->smooth        = true;
        pVppAfs->force24       = false;
        pVppAfs->tune          = false;
        break;
    case AFS_PRESET_ANIME: //映画/アニメ
        pVppAfs->method_switch = 64;
        pVppAfs->coeff_shift   = 128;
        pVppAfs->thre_shift    = 128;
        pVppAfs->thre_deint    = 48;
        pVppAfs->thre_Ymotion  = 112;
        pVppAfs->thre_Cmotion  = 224;
        pVppAfs->analyze       = 3;
        pVppAfs->shift         = true;
        pVppAfs->drop          = true;
        pVppAfs->smooth        = true;
        pVppAfs->force24       = false;
        pVppAfs->tune          = false;
        break;
    case AFS_PRESET_MIN_AFTERIMG:      //残像最小化
        pVppAfs->method_switch = 0;
        pVppAfs->coeff_shift   = 192;
        pVppAfs->thre_shift    = 128;
        pVppAfs->thre_deint    = 48;
        pVppAfs->thre_Ymotion  = 112;
        pVppAfs->thre_Cmotion  = 224;
        pVppAfs->analyze       = 4;
        pVppAfs->shift         = true;
        pVppAfs->drop          = true;
        pVppAfs->smooth        = true;
        pVppAfs->force24       = false;
        pVppAfs->tune          = false;
        break;
    case AFS_PRESET_FORCE24_SD:        //24fps固定
        pVppAfs->method_switch = 64;
        pVppAfs->coeff_shift   = 128;
        pVppAfs->thre_shift    = 128;
        pVppAfs->thre_deint    = 48;
        pVppAfs->thre_Ymotion  = 112;
        pVppAfs->thre_Cmotion  = 224;
        pVppAfs->analyze       = 3;
        pVppAfs->shift         = true;
        pVppAfs->drop          = true;
        pVppAfs->smooth        = false;
        pVppAfs->force24       = true;
        pVppAfs->tune          = false;
        break;
    case AFS_PRESET_FORCE24_HD:        //24fps固定 (HD)
        pVppAfs->method_switch = 92;
        pVppAfs->coeff_shift   = 192;
        pVppAfs->thre_shift    = 448;
        pVppAfs->thre_deint    = 48;
        pVppAfs->thre_Ymotion  = 112;
        pVppAfs->thre_Cmotion  = 224;
        pVppAfs->analyze       = 3;
        pVppAfs->shift         = true;
        pVppAfs->drop          = true;
        pVppAfs->smooth        = true;
        pVppAfs->force24       = true;
        pVppAfs->tune          = false;
        break;
    case AFS_PRESET_FORCE30:           //30fps固定
        pVppAfs->method_switch = 92;
        pVppAfs->coeff_shift   = 192;
        pVppAfs->thre_shift    = 448;
        pVppAfs->thre_deint    = 48;
        pVppAfs->thre_Ymotion  = 112;
        pVppAfs->thre_Cmotion  = 224;
        pVppAfs->analyze       = 3;
        pVppAfs->shift         = false;
        pVppAfs->drop          = false;
        pVppAfs->smooth        = false;
        pVppAfs->force24       = false;
        pVppAfs->tune          = false;
        break;
    default:
        break;
    }
}
int NVEncFilterAfs::read_afs_inifile(VppAfs *pVppAfs, const TCHAR *inifile) {
    if (!PathFileExists(inifile)) {
        return 1;
    }
    const auto filename = tchar_to_string(inifile).c_str();
    const auto section = AFS_STG_SECTION;

    pVppAfs->clip.top      = GetPrivateProfileIntA(section, AFS_STG_UP,               pVppAfs->clip.top,      filename);
    pVppAfs->clip.bottom   = GetPrivateProfileIntA(section, AFS_STG_BOTTOM,           pVppAfs->clip.bottom,   filename);
    pVppAfs->clip.left     = GetPrivateProfileIntA(section, AFS_STG_LEFT,             pVppAfs->clip.left,     filename);
    pVppAfs->clip.right    = GetPrivateProfileIntA(section, AFS_STG_RIGHT,            pVppAfs->clip.right,    filename);
    pVppAfs->method_switch = GetPrivateProfileIntA(section, AFS_STG_METHOD_WATERSHED, pVppAfs->method_switch, filename);
    pVppAfs->coeff_shift   = GetPrivateProfileIntA(section, AFS_STG_COEFF_SHIFT,      pVppAfs->coeff_shift,   filename);
    pVppAfs->thre_shift    = GetPrivateProfileIntA(section, AFS_STG_THRE_SHIFT,       pVppAfs->thre_shift,    filename);
    pVppAfs->thre_deint    = GetPrivateProfileIntA(section, AFS_STG_THRE_DEINT,       pVppAfs->thre_deint,    filename);
    pVppAfs->thre_Ymotion  = GetPrivateProfileIntA(section, AFS_STG_THRE_Y_MOTION,    pVppAfs->thre_Ymotion,  filename);
    pVppAfs->thre_Cmotion  = GetPrivateProfileIntA(section, AFS_STG_THRE_C_MOTION,    pVppAfs->thre_Cmotion,  filename);
    pVppAfs->analyze       = GetPrivateProfileIntA(section, AFS_STG_MODE,             pVppAfs->analyze,       filename);

    pVppAfs->shift    = 0 != GetPrivateProfileIntA(section, AFS_STG_FIELD_SHIFT,      pVppAfs->shift,         filename);
    pVppAfs->drop     = 0 != GetPrivateProfileIntA(section, AFS_STG_DROP,             pVppAfs->drop,          filename);
    pVppAfs->smooth   = 0 != GetPrivateProfileIntA(section, AFS_STG_SMOOTH,           pVppAfs->smooth,        filename);
    pVppAfs->force24  = 0 != GetPrivateProfileIntA(section, AFS_STG_FORCE24,          pVppAfs->force24,       filename);
    // GetPrivateProfileIntA(section, AFS_STG_DETECT_SC, fp->check[4], filename);
    pVppAfs->tune     = 0 != GetPrivateProfileIntA(section, AFS_STG_TUNE_MODE,         pVppAfs->tune,          filename);
    // GetPrivateProfileIntA(section, AFS_STG_LOG_SAVE, fp->check[6], filename);
    // GetPrivateProfileIntA(section, AFS_STG_TRACE_MODE, fp->check[7], filename);
    // GetPrivateProfileIntA(section, AFS_STG_REPLAY_MODE, fp->check[8], filename);
    // GetPrivateProfileIntA(section, AFS_STG_YUY2UPSAMPLE, fp->check[9], filename);
    // GetPrivateProfileIntA(section, AFS_STG_THROUGH_MODE, fp->check[10], filename);

    // GetPrivateProfileIntA(section, AFS_STG_PROC_MODE, g_afs.ex_data.proc_mode, filename);
    return 0;
}

NVENCSTATUS NVEncFilterAfs::check_param(shared_ptr<NVEncFilterParamAfs> pAfsParam) {
    if (pAfsParam->frameOut.height <= 0 || pAfsParam->frameOut.width <= 0) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid parameter.\n"));
        return NV_ENC_ERR_INVALID_PARAM;
    }
    if (pAfsParam->afs.clip.top < 0 || pAfsParam->afs.clip.top >= pAfsParam->frameOut.height) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid parameter (clip.top).\n"));
        return NV_ENC_ERR_INVALID_PARAM;
    }
    if (pAfsParam->afs.clip.bottom < 0 || pAfsParam->afs.clip.bottom >= pAfsParam->frameOut.height) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid parameter (clip.bottom).\n"));
        return NV_ENC_ERR_INVALID_PARAM;
    }
    if (pAfsParam->afs.clip.top + pAfsParam->afs.clip.bottom >= pAfsParam->frameOut.height) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid parameter (clip.top + clip.bottom).\n"));
        return NV_ENC_ERR_INVALID_PARAM;
    }
    if (pAfsParam->afs.clip.left < 0 || pAfsParam->afs.clip.left >= pAfsParam->frameOut.width) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid parameter (clip.left).\n"));
        return NV_ENC_ERR_INVALID_PARAM;
    }
    if (pAfsParam->afs.clip.right < 0 || pAfsParam->afs.clip.right >= pAfsParam->frameOut.width) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid parameter (clip.right).\n"));
        return NV_ENC_ERR_INVALID_PARAM;
    }
    if (pAfsParam->afs.clip.left + pAfsParam->afs.clip.right >= pAfsParam->frameOut.width) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid parameter (clip.left + clip.right).\n"));
        return NV_ENC_ERR_INVALID_PARAM;
    }
    if (pAfsParam->afs.method_switch < 0 || pAfsParam->afs.method_switch > 256) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid parameter (method_switch).\n"));
        return NV_ENC_ERR_INVALID_PARAM;
    }
    if (pAfsParam->afs.coeff_shift < 0 || pAfsParam->afs.coeff_shift > 256) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid parameter (coeff_shift).\n"));
        return NV_ENC_ERR_INVALID_PARAM;
    }
    if (pAfsParam->afs.thre_shift < 0 || pAfsParam->afs.thre_shift > 1024) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid parameter (thre_shift).\n"));
        return NV_ENC_ERR_INVALID_PARAM;
    }
    if (pAfsParam->afs.thre_deint < 0 || pAfsParam->afs.thre_deint > 1024) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid parameter (thre_deint).\n"));
        return NV_ENC_ERR_INVALID_PARAM;
    }
    if (pAfsParam->afs.thre_Ymotion < 0 || pAfsParam->afs.thre_Ymotion > 1024) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid parameter (thre_Ymotion).\n"));
        return NV_ENC_ERR_INVALID_PARAM;
    }
    if (pAfsParam->afs.thre_Cmotion < 0 || pAfsParam->afs.thre_Cmotion > 1024) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid parameter (thre_Cmotion).\n"));
        return NV_ENC_ERR_INVALID_PARAM;
    }
    if (pAfsParam->afs.analyze < 0 || pAfsParam->afs.analyze > 5) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid parameter (analyze).\n"));
        return NV_ENC_ERR_INVALID_PARAM;
    }
    if (!pAfsParam->afs.shift) {
        AddMessage(RGY_LOG_WARN, _T("shift was off, so drop and smooth will also be off.\n"));
        pAfsParam->afs.drop = false;
        pAfsParam->afs.smooth = false;
    }
    return NV_ENC_SUCCESS;
}

NVENCSTATUS NVEncFilterAfs::init(shared_ptr<NVEncFilterParam> pParam, shared_ptr<RGYLog> pPrintMes) {
    NVENCSTATUS sts = NV_ENC_SUCCESS;
    m_pPrintMes = pPrintMes;
    auto pAfsParam = std::dynamic_pointer_cast<NVEncFilterParamAfs>(pParam);
    if (!pAfsParam) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid parameter type.\n"));
        return NV_ENC_ERR_INVALID_PARAM;
    }
    //パラメータチェック
    if (check_param(pAfsParam) != NV_ENC_SUCCESS) {
        return NV_ENC_ERR_INVALID_PARAM;
    }

    auto cudaerr = AllocFrameBuf(pAfsParam->frameOut, 1);
    if (cudaerr != CUDA_SUCCESS) {
        AddMessage(RGY_LOG_ERROR, _T("failed to allocate memory: %s.\n"), char_to_tstring(cudaGetErrorName(cudaerr)).c_str());
        return NV_ENC_ERR_OUT_OF_MEMORY;
    }
    pAfsParam->frameOut.pitch = m_pFrameBuf[0]->frame.pitch;

    if (CUDA_SUCCESS != (cudaerr = m_source.alloc(pAfsParam->frameOut))) {
        AddMessage(RGY_LOG_ERROR, _T("failed to allocate memory: %s.\n"), char_to_tstring(cudaGetErrorName(cudaerr)).c_str());
        return NV_ENC_ERR_OUT_OF_MEMORY;
    }
    if (CUDA_SUCCESS != (cudaerr = m_scan.alloc(pAfsParam->frameOut))) {
        AddMessage(RGY_LOG_ERROR, _T("failed to allocate memory: %s.\n"), char_to_tstring(cudaGetErrorName(cudaerr)).c_str());
        return NV_ENC_ERR_OUT_OF_MEMORY;
    }
    if (CUDA_SUCCESS != (cudaerr = m_stripe.alloc(pAfsParam->frameOut))) {
        AddMessage(RGY_LOG_ERROR, _T("failed to allocate memory: %s.\n"), char_to_tstring(cudaGetErrorName(cudaerr)).c_str());
        return NV_ENC_ERR_OUT_OF_MEMORY;
    }
    m_stream = std::unique_ptr<cudaStream_t, cudastream_deleter>(new cudaStream_t(), cudastream_deleter());
    if (CUDA_SUCCESS != (cudaerr = cudaStreamCreateWithFlags(m_stream.get(), cudaStreamNonBlocking))) {
        AddMessage(RGY_LOG_ERROR, _T("failed to cudaStreamCreateWithFlags: %s.\n"), char_to_tstring(cudaGetErrorName(cudaerr)).c_str());
        return NV_ENC_ERR_OUT_OF_MEMORY;
    }

    pAfsParam->frameOut.picstruct = RGY_PICSTRUCT_FRAME;
    m_nFrame = 0;
    m_nPts = 0;
    m_nPathThrough &= (~(FILTER_PATHTHROUGH_PICSTRUCT | FILTER_PATHTHROUGH_TIMESTAMP | FILTER_PATHTHROUGH_FLAGS));

    if (pAfsParam->afs.timecode) {
        const tstring tc_filename = pAfsParam->outFilename + _T(".timecode.txt");
        if (open_timecode(tc_filename)) {
            errno_t error = errno;
            AddMessage(RGY_LOG_ERROR, _T("failed to open timecode file \"%s\": %s.\n"), tc_filename.c_str(), _tcserror(error));
            return NV_ENC_ERR_GENERIC; // Couldn't open file
        }
    }

    if (pAfsParam->afs.log) {
        const tstring log_filename = pAfsParam->outFilename + _T(".afslog.csv");
        if (m_streamsts.open_log(log_filename)) {
            errno_t error = errno;
            AddMessage(RGY_LOG_ERROR, _T("failed to open timecode file \"%s\": %s.\n"), log_filename.c_str(), _tcserror(error));
            return NV_ENC_ERR_GENERIC; // Couldn't open file
        }
    }

#define ON_OFF(b) ((b) ? _T("on") : _T("off"))
    m_sFilterInfo = strsprintf(
        _T("afs: clip(T %d, B %d, L %d, R %d), switch %d, coeff_shift %d\n")
        _T("                    thre(shift %d, deint %d, Ymotion %d, Cmotion %d)\n")
        _T("                    analyze %d, shift %s, drop %s, smooth %s, force24 %s\n")
        _T("                    tune %s, tb_order %d(%s), rff %s, timecode %s, log %s"),
        pAfsParam->afs.clip.top, pAfsParam->afs.clip.bottom , pAfsParam->afs.clip.left, pAfsParam->afs.clip.right,
        pAfsParam->afs.method_switch, pAfsParam->afs.coeff_shift,
        pAfsParam->afs.thre_shift, pAfsParam->afs.thre_deint, pAfsParam->afs.thre_Ymotion, pAfsParam->afs.thre_Cmotion,
        pAfsParam->afs.analyze, ON_OFF(pAfsParam->afs.shift), ON_OFF(pAfsParam->afs.drop), ON_OFF(pAfsParam->afs.smooth), ON_OFF(pAfsParam->afs.force24),
        ON_OFF(pAfsParam->afs.tune), pAfsParam->afs.tb_order, pAfsParam->afs.tb_order ? _T("tff") : _T("bff"), ON_OFF(pAfsParam->afs.rff), ON_OFF(pAfsParam->afs.timecode), ON_OFF(pAfsParam->afs.log));
#undef ON_OFF
    m_pParam = pParam;
    return sts;
}

bool NVEncFilterAfs::scan_frame_result_cached(int frame, const VppAfs *pAfsPrm) {
    auto sp = m_scan.get(frame);
    const int mode = pAfsPrm->analyze == 0 ? 0 : 1;
    return sp->status > 0 && sp->frame == frame && sp->tb_order == pAfsPrm->tb_order && sp->thre_shift == pAfsPrm->thre_shift &&
        ((mode == 0) ||
        (mode == 1 && sp->mode == 1 && sp->thre_deint == pAfsPrm->thre_deint && sp->thre_Ymotion == pAfsPrm->thre_Ymotion && sp->thre_Cmotion == pAfsPrm->thre_Cmotion));
}

cudaError_t NVEncFilterAfs::scan_frame(int iframe, int force, const NVEncFilterParamAfs *pAfsPrm) {
    if (!force && scan_frame_result_cached(iframe, &pAfsPrm->afs)) {
        return cudaSuccess;
    }
    auto p1 = m_source.get(iframe-1);
    auto p0 = m_source.get(iframe);
    auto sp = m_scan.get(iframe);

    const int mode = pAfsPrm->afs.analyze == 0 ? 0 : 1;
    m_stripe.expire(iframe - 1);
    m_stripe.expire(iframe);
    sp->status = 1;
    sp->frame = iframe, sp->mode = mode, sp->tb_order = pAfsPrm->afs.tb_order;
    sp->thre_shift = pAfsPrm->afs.thre_shift, sp->thre_deint = pAfsPrm->afs.thre_deint;
    sp->thre_Ymotion = pAfsPrm->afs.thre_Ymotion, sp->thre_Cmotion = pAfsPrm->afs.thre_Cmotion;
    sp->clip.top = sp->clip.bottom = sp->clip.left = sp->clip.right = -1;
    auto cudaerr = analyze_stripe(p0, p1, sp, &m_count_motion, pAfsPrm, cudaStreamDefault);
    if (cudaerr != cudaSuccess) {
        AddMessage(RGY_LOG_ERROR, _T("failed analyze_stripe: %s.\n"), char_to_tstring(cudaGetErrorName(cudaerr)).c_str());
        return cudaerr;
    }

    cudaerr = count_motion(sp, &pAfsPrm->afs.clip);
    if (cudaerr != cudaSuccess) {
        AddMessage(RGY_LOG_ERROR, _T("failed analyze_stripe: %s.\n"), char_to_tstring(cudaGetErrorName(cudaerr)).c_str());
        return cudaerr;
    }
    return cudaerr;
}

cudaError_t NVEncFilterAfs::count_motion(AFS_SCAN_DATA *sp, const AFS_SCAN_CLIP *clip) {
    sp->clip = *clip;

    //cudaStreamSynchronize(*m_stream.get());
    auto cudaerr = m_count_motion.copyDtoH();
    if (cudaerr != cudaSuccess) {
        AddMessage(RGY_LOG_ERROR, _T("failed m_count_motion.copyDtoH: %s.\n"), char_to_tstring(cudaGetErrorName(cudaerr)).c_str());
        return cudaerr;
    }

    const int nSize = (int)(m_count_motion.nSize / sizeof(uint32_t));
    int count0 = 0;
    int count1 = 0;
    uint32_t *ptrCount = (uint32_t *)m_count_motion.ptrHost;
    for (int i = 0; i < nSize; i++) {
        uint32_t count = ptrCount[i];
        count0 += count & 0xffff;
        count1 += count >> 16;
    }
    sp->ff_motion = count0;
    sp->lf_motion = count1;
    //AddMessage(RGY_LOG_INFO, _T("count_motion[%6d]: %6d - %6d (ff,lf)"), sp->frame, sp->ff_motion, sp->lf_motion);
#if 0
    uint8_t *ptr = nullptr;
    if (cudaSuccess != (cudaerr = cudaMallocHost(&ptr, sp->map.frame.pitch * sp->map.frame.height))) {
        AddMessage(RGY_LOG_ERROR, _T("failed cudaMallocHost: %s.\n"), char_to_tstring(cudaGetErrorName(cudaerr)).c_str());
        return cudaerr;
    }
    if (cudaSuccess != (cudaerr = cudaMemcpy2D(ptr, sp->map.frame.pitch, sp->map.frame.ptr, sp->map.frame.pitch, sp->map.frame.width, sp->map.frame.height, cudaMemcpyDeviceToHost))) {
        AddMessage(RGY_LOG_ERROR, _T("failed cudaMemcpy2D: %s.\n"), char_to_tstring(cudaGetErrorName(cudaerr)).c_str());
        return cudaerr;
    }

    int motion_count[2] = { 0, 0 };
    afs_get_motion_count_simd(motion_count, ptr, &sp->clip, sp->map.frame.pitch, sp->map.frame.width, sp->map.frame.height, sp->tb_order);
    AddMessage((count0 == motion_count[0] && count1 == motion_count[1]) ? RGY_LOG_INFO : RGY_LOG_ERROR, _T("count_motion(ret, debug) = (%6d, %6d) / (%6d, %6d)\n"), count0, motion_count[0], count1, motion_count[1]);
    if (cudaSuccess != (cudaerr = cudaFreeHost(ptr))) {
        AddMessage(RGY_LOG_ERROR, _T("failed cudaFreeHost: %s.\n"), char_to_tstring(cudaGetErrorName(cudaerr)).c_str());
        return cudaerr;
    }
#endif
    return cudaerr;
}

cudaError_t NVEncFilterAfs::get_stripe_info(int iframe, int mode, const NVEncFilterParamAfs *pAfsPrm) {
    AFS_STRIPE_DATA *sp = m_stripe.get(iframe);
    if (sp->status > mode && sp->status < 3 && sp->frame == iframe)
        return cudaSuccess;

    AFS_SCAN_DATA *sp0 = m_scan.get(iframe);
    AFS_SCAN_DATA *sp1 = m_scan.get(iframe + 1);
    auto cudaerr = merge_scan(sp, sp0, sp1, &m_count_stripe, pAfsPrm, cudaStreamDefault);
    if (cudaerr != cudaSuccess) {
        AddMessage(RGY_LOG_ERROR, _T("failed merge_scan: %s.\n"), char_to_tstring(cudaGetErrorName(cudaerr)).c_str());
        return cudaerr;
    }

    sp->status = 2;
    sp->frame = iframe;

    if (cudaSuccess != (cudaerr = count_stripe(sp, &pAfsPrm->afs.clip, pAfsPrm->afs.tb_order))) {
        AddMessage(RGY_LOG_ERROR, _T("failed count_stripe: %s.\n"), char_to_tstring(cudaGetErrorName(cudaerr)).c_str());
        return cudaerr;
    }
    return cudaerr;
}

cudaError_t NVEncFilterAfs::count_stripe(AFS_STRIPE_DATA *sp, const AFS_SCAN_CLIP *clip, int tb_order) {
    //cudaStreamSynchronize(*m_stream.get());
    auto cudaerr = m_count_stripe.copyDtoH();
    if (cudaerr != cudaSuccess) {
        AddMessage(RGY_LOG_ERROR, _T("failed m_count_stripe.copyDtoH: %s.\n"), char_to_tstring(cudaGetErrorName(cudaerr)).c_str());
        return cudaerr;
    }

    const int nSize = (int)(m_count_stripe.nSize / sizeof(uint32_t));
    int count0 = 0;
    int count1 = 0;
    uint32_t *ptrCount = (uint32_t *)m_count_stripe.ptrHost;
    for (int i = 0; i < nSize; i++) {
        uint32_t count = ptrCount[i];
        count0 += count & 0xffff;
        count1 += count >> 16;
    }
    sp->count0 = count0;
    sp->count1 = count1;
    //AddMessage(RGY_LOG_INFO, _T("count_stripe[%6d]: %6d - %6d"), sp->frame, count0, count1);
#if 0
    uint8_t *ptr = nullptr;
    if (cudaSuccess != (cudaerr = cudaMallocHost(&ptr, sp->map.frame.pitch * sp->map.frame.height))) {
        AddMessage(RGY_LOG_ERROR, _T("failed cudaMallocHost: %s.\n"), char_to_tstring(cudaGetErrorName(cudaerr)).c_str());
        return cudaerr;
    }
    if (cudaSuccess != (cudaerr = cudaMemcpy2D(ptr, sp->map.frame.pitch, sp->map.frame.ptr, sp->map.frame.pitch, sp->map.frame.width, sp->map.frame.height, cudaMemcpyDeviceToHost))) {
        AddMessage(RGY_LOG_ERROR, _T("failed cudaMemcpy2D: %s.\n"), char_to_tstring(cudaGetErrorName(cudaerr)).c_str());
        return cudaerr;
    }

    int stripe_count[2] = { 0, 0 };
    afs_get_stripe_count_simd(stripe_count, ptr, clip, sp->map.frame.pitch, sp->map.frame.width, sp->map.frame.height, tb_order);
    AddMessage((count0 == stripe_count[0] && count1 == stripe_count[1]) ? RGY_LOG_INFO : RGY_LOG_ERROR, _T("count_stripe(ret, debug) = (%6d, %6d) / (%6d, %6d)\n"), count0, stripe_count[0], count1, stripe_count[1]);
    if (cudaSuccess != (cudaerr = cudaFreeHost(ptr))) {
        AddMessage(RGY_LOG_ERROR, _T("failed cudaFreeHost: %s.\n"), char_to_tstring(cudaGetErrorName(cudaerr)).c_str());
        return cudaerr;
    }
#endif
    return cudaerr;
}

int NVEncFilterAfs::detect_telecine_cross(int iframe, int coeff_shift) {
    using std::max;
    const AFS_SCAN_DATA *sp1 = m_scan.get(iframe - 1);
    const AFS_SCAN_DATA *sp2 = m_scan.get(iframe + 0);
    const AFS_SCAN_DATA *sp3 = m_scan.get(iframe + 1);
    const AFS_SCAN_DATA *sp4 = m_scan.get(iframe + 2);
    int shift = 0;

    if (max(absdiff(sp1->lf_motion + sp2->lf_motion, sp2->ff_motion),
        absdiff(sp3->ff_motion + sp4->ff_motion, sp3->lf_motion)) * coeff_shift >
        max3(absdiff(sp1->ff_motion + sp2->ff_motion, sp1->lf_motion),
            absdiff(sp2->ff_motion + sp3->ff_motion, sp2->lf_motion),
            absdiff(sp3->lf_motion + sp4->lf_motion, sp4->ff_motion)) * 256)
        if (max(sp2->lf_motion, sp3->ff_motion) * coeff_shift > sp2->ff_motion * 256)
            shift = 1;

    if (max(absdiff(sp1->lf_motion + sp2->lf_motion, sp2->ff_motion),
        absdiff(sp3->ff_motion + sp4->ff_motion, sp3->lf_motion)) * coeff_shift >
        max3(absdiff(sp1->ff_motion + sp2->ff_motion, sp1->lf_motion),
            absdiff(sp2->lf_motion + sp3->lf_motion, sp3->ff_motion),
            absdiff(sp3->lf_motion + sp4->lf_motion, sp4->ff_motion)) * 256)
        if (max(sp2->lf_motion, sp3->ff_motion) * coeff_shift > sp3->lf_motion * 256)
            shift = 1;

    return shift;
}

cudaError_t NVEncFilterAfs::analyze_frame(int iframe, const NVEncFilterParamAfs *pAfsPrm, int reverse[4], int assume_shift[4], int result_stat[4]) {
    for (int i = 0; i < 4; i++) {
        assume_shift[i] = detect_telecine_cross(iframe + i, pAfsPrm->afs.coeff_shift);
    }

    AFS_SCAN_DATA *scp = m_scan.get(iframe);
    const int scan_w = m_pParam->frameIn.width;
    const int scan_h = m_pParam->frameIn.height;
    int total = 0;
    if (scan_h - scp->clip.bottom - ((scan_h - scp->clip.top - scp->clip.bottom) & 1) > scp->clip.top && scan_w - scp->clip.right > scp->clip.left)
        total = (scan_h - scp->clip.bottom - ((scan_h - scp->clip.top - scp->clip.bottom) & 1) - scp->clip.top) * (scan_w - scp->clip.right - scp->clip.left);
    const int threshold = (total * pAfsPrm->afs.method_switch) >> 12;

    for (int i = 0; i < 4; i++) {
        auto cudaerr = get_stripe_info(iframe + i, 0, pAfsPrm);
        if (cudaerr != cudaSuccess) {
            AddMessage(RGY_LOG_ERROR, _T("failed on get_stripe_info(iframe=%d): %s.\n"), iframe + i, char_to_tstring(cudaGetErrorName(cudaerr)).c_str());
            return cudaerr;
        }
        AFS_STRIPE_DATA *stp = m_stripe.get(iframe + i);
        result_stat[i] = (stp->count0 * pAfsPrm->afs.coeff_shift > stp->count1 * 256) ? 1 : 0;
        if (threshold > stp->count1 && threshold > stp->count0)
            result_stat[i] += 2;
    }

    uint8_t status = AFS_STATUS_DEFAULT;
    if (result_stat[0] & 2)
        status |= assume_shift[0] ? AFS_FLAG_SHIFT0 : 0;
    else
        status |= (result_stat[0] & 1) ? AFS_FLAG_SHIFT0 : 0;
    if (reverse[0]) status ^= AFS_FLAG_SHIFT0;

    if (result_stat[1] & 2)
        status |= assume_shift[1] ? AFS_FLAG_SHIFT1 : 0;
    else
        status |= (result_stat[1] & 1) ? AFS_FLAG_SHIFT1 : 0;
    if (reverse[1]) status ^= AFS_FLAG_SHIFT1;

    if (result_stat[2] & 2)
        status |= assume_shift[2] ? AFS_FLAG_SHIFT2 : 0;
    else
        status |= (result_stat[2] & 1) ? AFS_FLAG_SHIFT2 : 0;
    if (reverse[2]) status ^= AFS_FLAG_SHIFT2;

    if (result_stat[3] & 2)
        status |= assume_shift[3] ? AFS_FLAG_SHIFT3 : 0;
    else
        status |= (result_stat[3] & 1) ? AFS_FLAG_SHIFT3 : 0;
    if (reverse[3]) status ^= AFS_FLAG_SHIFT3;

    const auto& frameinfo = m_source.get(iframe)->frame;
    if (!interlaced(frameinfo)) {
        status |= AFS_FLAG_PROGRESSIVE;
        if (frameinfo.flags & RGY_FRAME_FLAG_RFF) status |= AFS_FLAG_RFF;
    }
    if (pAfsPrm->afs.drop) {
        if (interlaced(frameinfo)) status |= AFS_FLAG_FRAME_DROP;
        if (pAfsPrm->afs.smooth) status |= AFS_FLAG_SMOOTHING;
    }
    if (pAfsPrm->afs.force24) status |= AFS_FLAG_FORCE24;
    if (iframe < 1) status &= AFS_MASK_SHIFT0;

    m_status[iframe] = status;
    return cudaSuccess;
}

NVENCSTATUS NVEncFilterAfs::run_filter(const FrameInfo *pInputFrame, FrameInfo **ppOutputFrames, int *pOutputFrameNum) {
    NVENCSTATUS sts = NV_ENC_SUCCESS;

    auto pAfsParam = std::dynamic_pointer_cast<NVEncFilterParamAfs>(m_pParam);
    if (!pAfsParam) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid parameter type.\n"));
        return NV_ENC_ERR_INVALID_PARAM;
    }

    const int iframe = m_source.inframe();
    if (pInputFrame->ptr == nullptr && m_nFrame >= iframe) {
        //終了
        *pOutputFrameNum = 0;
        ppOutputFrames[0] = nullptr;
        return sts;
    } else if (pInputFrame->ptr != nullptr) {
        //エラーチェック
        const auto memcpyKind = getCudaMemcpyKind(pInputFrame->deivce_mem, m_pFrameBuf[0]->frame.deivce_mem);
        if (memcpyKind != cudaMemcpyDeviceToDevice) {
            AddMessage(RGY_LOG_ERROR, _T("only supported on device memory.\n"));
            return NV_ENC_ERR_UNSUPPORTED_PARAM;
        }
        if (m_pParam->frameOut.csp != m_pParam->frameIn.csp) {
            AddMessage(RGY_LOG_ERROR, _T("csp does not match.\n"));
            return NV_ENC_ERR_UNSUPPORTED_PARAM;
        }
        //sourceキャッシュにコピー
        auto cudaerr = m_source.add(pInputFrame, cudaStreamDefault);
        if (cudaerr != cudaSuccess) {
            AddMessage(RGY_LOG_ERROR, _T("failed to add frame to sorce buffer: %s.\n"), char_to_tstring(cudaGetErrorName(cudaerr)).c_str());
            return NV_ENC_ERR_OUT_OF_MEMORY;
        }
        if (iframe == 0) {
            // scan_frame(p1 = -2, p0 = -1)のscan_frameも必要
            if (cudaSuccess != (cudaerr = scan_frame(iframe-1, false, pAfsParam.get()))) {
                AddMessage(RGY_LOG_ERROR, _T("failed on scan_frame(iframe-1=%d): %s.\n"), iframe-1, char_to_tstring(cudaGetErrorName(cudaerr)).c_str());
                return NV_ENC_ERR_INVALID_CALL;
            }
        }
        if (cudaSuccess != (cudaerr = scan_frame(iframe, false, pAfsParam.get()))) {
            AddMessage(RGY_LOG_ERROR, _T("failed on scan_frame(iframe=%d): %s.\n"), iframe, char_to_tstring(cudaGetErrorName(cudaerr)).c_str());
            return NV_ENC_ERR_INVALID_CALL;
        }
    }

    if (iframe >= 5) {
        int reverse[4] = { 0 }, assume_shift[4] = { 0 }, result_stat[4] = { 0 };
        auto cudaerr = analyze_frame(iframe - 5, pAfsParam.get(), reverse, assume_shift, result_stat);
        if (cudaerr != cudaSuccess) {
            AddMessage(RGY_LOG_ERROR, _T("failed on scan_frame(iframe=%d): %s.\n"), iframe - 5, char_to_tstring(cudaGetErrorName(cudaerr)).c_str());
            return NV_ENC_ERR_INVALID_CALL;
        }
    }
    static const int preread_len = 3;
    //十分な数のフレームがたまった、あるいはdrainモードならフレームを出力
    if (iframe >= (5+preread_len) || pInputFrame == nullptr) {
        int reverse[4] = { 0 }, assume_shift[4] = { 0 }, result_stat[4] = { 0 };

        //m_streamsts.get_durationを呼ぶには、3フレーム先までstatusをセットする必要がある
        //そのため、analyze_frameを使って、3フレーム先までstatusを計算しておく
        for (int i = preread_len; i >= 0; i--) {
            //ここでは、これまで発行したanalyze_frameの結果からstatusの更新を行う(analyze_frameの内部で行われる)
            auto cudaerr = analyze_frame(m_nFrame + i, pAfsParam.get(), reverse, assume_shift, result_stat);
            if (cudaerr != cudaSuccess) {
                AddMessage(RGY_LOG_ERROR, _T("error on analyze_frame(m_nFrame=%d, iframe=%d): %s.\n"),
                    m_nFrame, m_nFrame + i, iframe, char_to_tstring(cudaGetErrorName(cudaerr)).c_str());
                return NV_ENC_ERR_INVALID_CALL;
            }
        }

        if (m_nFrame == 0) {
            //m_nFrame == 0のときは、下記がセットされていない
            for (int i = 0; i < preread_len; i++) {
                if (m_streamsts.set_status(i, m_status[i], i, m_source.get(i)->frame.timestamp) != 0) {
                    AddMessage(RGY_LOG_ERROR, _T("failed to set afs_status(%d).\n"), i);
                    return NV_ENC_ERR_INVALID_CALL;
                }
            }
        }
        //m_streamsts.get_durationを呼ぶには、3フレーム先までstatusをセットする必要がある
        if (m_streamsts.set_status(m_nFrame+preread_len, m_status[m_nFrame+preread_len], 0, m_source.get(m_nFrame+preread_len)->frame.timestamp) != 0) {
            AddMessage(RGY_LOG_ERROR, _T("failed to set afs_status(%d).\n"), m_nFrame+preread_len);
            return NV_ENC_ERR_INVALID_CALL;
        }
        const auto afs_duration = m_streamsts.get_duration(m_nFrame);
        if (afs_duration == afsStreamStatus::AFS_SSTS_DROP) {
            //出力フレームなし
            *pOutputFrameNum = 0;
            ppOutputFrames[0] = nullptr;
        } else if (afs_duration < 0) {
            AddMessage(RGY_LOG_ERROR, _T("invalid call for m_streamsts.get_duration(%d).\n"), m_nFrame);
            return NV_ENC_ERR_INVALID_CALL;
        } else {
            //出力先のフレーム
            CUFrameBuf *pOutFrame = nullptr;
            *pOutputFrameNum = 1;
            if (ppOutputFrames[0] == nullptr) {
                pOutFrame = m_pFrameBuf[m_nFrameIdx].get();
                ppOutputFrames[0] = &pOutFrame->frame;
                m_nFrameIdx = (m_nFrameIdx + 1) % m_pFrameBuf.size();
            }

            if (pAfsParam->afs.timecode) {
                write_timecode(m_nPts, pAfsParam->outTimebase);
            }

            pOutFrame->frame.flags = m_source.get(m_nFrame)->frame.flags & (~(RGY_FRAME_FLAG_RFF | RGY_FRAME_FLAG_RFF_COPY | RGY_FRAME_FLAG_RFF_BFF | RGY_FRAME_FLAG_RFF_TFF));
            pOutFrame->frame.picstruct = RGY_PICSTRUCT_FRAME;
            pOutFrame->frame.duration = rational_rescale(afs_duration, pAfsParam->inFps.inv() * rgy_rational<int>(1,4), pAfsParam->outTimebase);
            pOutFrame->frame.timestamp = m_nPts;
            m_nPts += pOutFrame->frame.duration;

            //出力するフレームを作成
            get_stripe_info(m_nFrame, 1, pAfsParam.get());
            cudaError_t cudaerr = cudaSuccess;
            auto sip_filtered = m_stripe.filter(m_nFrame, pAfsParam->afs.analyze, cudaStreamDefault, &cudaerr);
            if (sip_filtered == nullptr || cudaerr != CUDA_SUCCESS) {
                AddMessage(RGY_LOG_ERROR, _T("failed m_stripe.filter(m_nFrame=%d, iframe=%d): %s.\n"), m_nFrame, iframe - (5+preread_len), char_to_tstring(cudaGetErrorName(cudaerr)).c_str());
                return NV_ENC_ERR_INVALID_CALL;
            }

            if (interlaced(m_source.get(m_nFrame)->frame)) {
                cudaerr = synthesize(m_nFrame, pOutFrame, m_source.get(m_nFrame), m_source.get(m_nFrame-1), sip_filtered, pAfsParam.get(), cudaStreamDefault);
            } else {
                cudaerr = copy_frame(pOutFrame, m_source.get(m_nFrame), cudaStreamDefault);
            }
            if (cudaerr != cudaSuccess) {
                AddMessage(RGY_LOG_ERROR, _T("error on synthesize(m_nFrame=%d, iframe=%d): %s.\n"), m_nFrame, iframe - (5+preread_len), char_to_tstring(cudaGetErrorName(cudaerr)).c_str());
                return NV_ENC_ERR_INVALID_CALL;
            }
        }

        m_nFrame++;
    } else {
        //出力フレームなし
        *pOutputFrameNum = 0;
        ppOutputFrames[0] = nullptr;
    }
    return sts;
}

cudaError_t NVEncFilterAfs::copy_frame(CUFrameBuf *pOut, CUFrameBuf *p0, cudaStream_t stream) {
    const auto frameOutInfoEx = getFrameInfoExtra(&p0->frame);
    static const auto supportedCspYV12   = make_array<RGY_CSP>(RGY_CSP_YV12, RGY_CSP_YV12_09, RGY_CSP_YV12_10, RGY_CSP_YV12_12, RGY_CSP_YV12_14, RGY_CSP_YV12_16);
    static const auto supportedCspYUV444 = make_array<RGY_CSP>(RGY_CSP_YUV444, RGY_CSP_YUV444_09, RGY_CSP_YUV444_10, RGY_CSP_YUV444_12, RGY_CSP_YUV444_14, RGY_CSP_YUV444_16);
    auto cudaerr = cudaSuccess;
    if (std::find(supportedCspYV12.begin(), supportedCspYV12.end(), p0->frame.csp) != supportedCspYV12.end()) {
        //Y
        cudaerr = cudaMemcpy2DAsync((uint8_t *)pOut->frame.ptr, pOut->frame.pitch,
            (uint8_t *)p0->frame.ptr, p0->frame.pitch,
            frameOutInfoEx.width_byte, pOut->frame.height, cudaMemcpyDeviceToDevice, stream);
        if (cudaerr != cudaSuccess) return cudaerr;

        //Uフィールド
        cudaerr = cudaMemcpy2DAsync((uint8_t *)pOut->frame.ptr + pOut->frame.pitch * (pOut->frame.height + 0),
            pOut->frame.pitch * 2, //偶数ラインの展開
            (uint8_t *)p0->frame.ptr + p0->frame.pitch * p0->frame.height,
            p0->frame.pitch,
            frameOutInfoEx.width_byte >> 1, pOut->frame.height >> 2, cudaMemcpyDeviceToDevice, stream);
        if (cudaerr != cudaSuccess) return cudaerr;

        cudaerr = cudaMemcpy2DAsync((uint8_t *)pOut->frame.ptr + pOut->frame.pitch * (pOut->frame.height + 1),
            pOut->frame.pitch * 2, //奇数ラインの展開
            (uint8_t *)p0->frame.ptr + p0->frame.pitch * p0->frame.height * 5 / 4,
            p0->frame.pitch,
            frameOutInfoEx.width_byte >> 1, pOut->frame.height >> 2, cudaMemcpyDeviceToDevice, stream);
        if (cudaerr != cudaSuccess) return cudaerr;

        //Vフィールド
        cudaerr = cudaMemcpy2DAsync((uint8_t *)pOut->frame.ptr + pOut->frame.pitch * (pOut->frame.height * 3 / 2 + 0),
            pOut->frame.pitch * 2, //偶数ラインの展開
            (uint8_t *)p0->frame.ptr + p0->frame.pitch * p0->frame.height * 6 / 4,
            p0->frame.pitch,
            frameOutInfoEx.width_byte >> 1, pOut->frame.height >> 2, cudaMemcpyDeviceToDevice, stream);
        if (cudaerr != cudaSuccess) return cudaerr;

        cudaerr = cudaMemcpy2DAsync((uint8_t *)pOut->frame.ptr + pOut->frame.pitch * (pOut->frame.height * 3 / 2 + 1),
            pOut->frame.pitch * 2, //奇数ラインの展開
            (uint8_t *)p0->frame.ptr + p0->frame.pitch * p0->frame.height * 7 / 4,
            p0->frame.pitch,
            frameOutInfoEx.width_byte >> 1, pOut->frame.height >> 2, cudaMemcpyDeviceToDevice, stream);
        if (cudaerr != cudaSuccess) return cudaerr;

    } else if (std::find(supportedCspYUV444.begin(), supportedCspYUV444.end(), p0->frame.csp) != supportedCspYUV444.end()) {
        cudaerr = cudaMemcpy2DAsync((uint8_t *)pOut->frame.ptr, pOut->frame.pitch,
            (uint8_t *)p0->frame.ptr, p0->frame.pitch,
            frameOutInfoEx.width_byte, frameOutInfoEx.height_total, cudaMemcpyDeviceToDevice, stream);
    } else {
        cudaerr = cudaErrorNotSupported;
    }
    return cudaerr;
}

int NVEncFilterAfs::open_timecode(tstring tc_filename) {
    FILE *fp = NULL;
    if (_tfopen_s(&fp, tc_filename.c_str(), _T("w"))) {
        return 1;
    }
    m_fpTimecode = unique_ptr<FILE, fp_deleter>(fp, fp_deleter());
    fprintf(m_fpTimecode.get(), "# timecode format v2\n");
    return 0;
}

void NVEncFilterAfs::write_timecode(int64_t pts, const rgy_rational<int>& timebase) {
    if (pts > 0) {
        fprintf(m_fpTimecode.get(), "%.6lf\n", pts * timebase.qdouble() * 1000.0);
    }
}

void NVEncFilterAfs::close() {
    m_stream.reset();
    m_nFrame = 0;
    m_pFrameBuf.clear();
    m_source.clear();
    m_scan.clear();
    m_stripe.clear();
    m_status.clear();
    m_count_motion.clear();
    m_count_stripe.clear();
    m_fpTimecode.reset();
}

static inline BOOL is_latter_field(int pos_y, int tb_order) {
    return ((pos_y & 1) == tb_order);
}

static void afs_get_stripe_count_simd(int *stripe_count, const uint8_t *ptr, const AFS_SCAN_CLIP *clip, int pitch, int scan_w, int scan_h, int tb_order) {
    static const _declspec(align(16)) BYTE STRIPE_COUNT_CHECK_MASK[][16] = {
        { 0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50 },
        { 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60 },
    };
    const int y_fin = scan_h - clip->bottom - ((scan_h - clip->top - clip->bottom) & 1);
    const uint32_t check_mask[2] = { 0x50, 0x60 };
    __m128i xZero = _mm_setzero_si128();
    __m128i xMask, x0, x1;
    for (int pos_y = clip->top; pos_y < y_fin; pos_y++) {
        const uint8_t *sip = ptr + pos_y * pitch + clip->left;
        const int first_field_flag = !is_latter_field(pos_y, tb_order);
        xMask = _mm_load_si128((const __m128i*)STRIPE_COUNT_CHECK_MASK[first_field_flag]);
        const int x_count = scan_w - clip->right - clip->left;
        const uint8_t *sip_fin = sip + (x_count & ~31);
        for (; sip < sip_fin; sip += 32) {
            x0 = _mm_loadu_si128((const __m128i*)(sip +  0));
            x1 = _mm_loadu_si128((const __m128i*)(sip + 16));
            x0 = _mm_and_si128(x0, xMask);
            x1 = _mm_and_si128(x1, xMask);
            x0 = _mm_cmpeq_epi8(x0, xZero);
            x1 = _mm_cmpeq_epi8(x1, xZero);
            uint32_t count0 = _mm_movemask_epi8(x0);
            uint32_t count1 = _mm_movemask_epi8(x1);
            stripe_count[first_field_flag] += popcnt32(((count1 << 16) | count0));
        }
        if (x_count & 16) {
            x0 = _mm_loadu_si128((const __m128i*)sip);
            x0 = _mm_and_si128(x0, xMask);
            x0 = _mm_cmpeq_epi8(x0, xZero);
            uint32_t count0 = _mm_movemask_epi8(x0);
            stripe_count[first_field_flag] += popcnt32(count0);
            sip += 16;
        }
        sip_fin = sip + (x_count & 15);
        for (; sip < sip_fin; sip++)
            stripe_count[first_field_flag] += (!(*sip & check_mask[first_field_flag]));
    }
}

static void afs_get_motion_count_simd(int *motion_count, const uint8_t *ptr, const AFS_SCAN_CLIP *clip, int pitch, int scan_w, int scan_h, int tb_order) {
    const int y_fin = scan_h - clip->bottom - ((scan_h - clip->top - clip->bottom) & 1);
    __m128i xMotion = _mm_set1_epi8(0x40);
    __m128i x0, x1;
    for (int pos_y = clip->top; pos_y < y_fin; pos_y++) {
        const uint8_t *sip = ptr + pos_y * pitch + clip->left;
        const int is_latter_feild = is_latter_field(pos_y, tb_order);
        const int x_count = scan_w - clip->right - clip->left;
        const uint8_t *sip_fin = sip + (x_count & ~31);
        for (; sip < sip_fin; sip += 32) {
            x0 = _mm_loadu_si128((const __m128i*)(sip +  0));
            x1 = _mm_loadu_si128((const __m128i*)(sip + 16));
            x0 = _mm_andnot_si128(x0, xMotion);
            x1 = _mm_andnot_si128(x1, xMotion);
            x0 = _mm_cmpeq_epi8(x0, xMotion);
            x1 = _mm_cmpeq_epi8(x1, xMotion);
            uint32_t count0 = _mm_movemask_epi8(x0);
            uint32_t count1 = _mm_movemask_epi8(x1);
            motion_count[is_latter_feild] += popcnt32(((count1 << 16) | count0));
        }
        if (x_count & 16) {
            x0 = _mm_loadu_si128((const __m128i*)sip);
            x0 = _mm_andnot_si128(x0, xMotion);
            x0 = _mm_cmpeq_epi8(x0, xMotion);
            uint32_t count0 = _mm_movemask_epi8(x0);
            motion_count[is_latter_feild] += popcnt32(count0);
            sip += 16;
        }
        sip_fin = sip + (x_count & 15);
        for (; sip < sip_fin; sip++)
            motion_count[is_latter_feild] += ((~*sip & 0x40) >> 6);
    }
}