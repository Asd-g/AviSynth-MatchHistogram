#include <algorithm>
#include <cstdint>
#include <cstring>

#include <avisynth.h>
#include <avs\minmax.h>

static inline int IntDiv(int x, int y) {
    return ((x < 0) ^ (y < 0)) ? ((x - (y >> 1)) / y)
        : ((x + (y >> 1)) / y);
}


static inline void fillPlane(uint8_t* data, int width, int height, int stride, int value) {
    for (int y = 0; y < height; y++) {
        memset(data, value, width);

        data += stride;
    }
}


class CurveData {
private:
    unsigned int sum[256];
    unsigned int div[256];
    unsigned char curve[256];

public:
    void Create(const uint8_t* ptr1, const uint8_t* ptr2, int width, int height, int stride, int stride1, bool raw, int smoothing_window) {
        // Clear data
        for (int i = 0; i < 256; i++) {
            sum[i] = 0;
            div[i] = 0;
        }

        // Populate data
        for (int h = 0; h < height; h++) {
            for (int w = 0; w < width; w++) {
                sum[ptr1[w]] += ptr2[w];
                div[ptr1[w]] += 1;
            }
            ptr1 += stride;
            ptr2 += stride1;
        }

        // Raw curve
        for (int i = 0; i < 256; i++) {
            if (div[i] != 0) {
                curve[i] = IntDiv(sum[i], div[i]);
            }
            else {
                curve[i] = 0;
            }
        }

        if (!raw) {
            int flat = -1;
            for (int i = 0; i < 256; i++) {
                if (div[i] != 0) {
                    if (flat == -1) {
                        flat = i;
                    }
                    else {
                        flat = -1;
                        break;
                    }
                }
            }

            if (flat != -1) {
                // Uniform color
                for (int i = 0; i < 256; i++) {
                    curve[i] = curve[flat];
                }
            }
            else {
                for (int i = 0; i < 256; i++) {
                    if (div[i] == 0) {
                        int prev = -1;
                        for (int p = i - 1; p >= 0; p--) {
                            if (div[p] != 0) {
                                prev = p;
                                break;
                            }
                        }

                        int next = -1;
                        for (int n = i + 1; n < 256; n++) {
                            if (div[n] != 0) {
                                next = n;
                                break;
                            }
                        }

                        // Fill missing
                        if (prev != -1 && next != -1) {
                            curve[i] = std::min(std::max(curve[prev] + IntDiv((i - prev) * (curve[next] - curve[prev]), (next - prev)), 0), 255);
                            sum[i] = curve[i];
                            div[i] = 1;
                        }
                    }
                }

                while (div[0] == 0 || div[255] == 0) {
                    if (div[0] == 0) {
                        int first = -1;
                        for (int f = 0; f < 256; f++) {
                            if (div[f] != 0) {
                                first = f;
                                break;
                            }
                        }

                        // Extend bottom
                        for (int i = 0; i < first; i++) {
                            if (first * 2 - i <= 255) {
                                if (div[first * 2 - i] != 0) {
                                    curve[i] = std::min(std::max(curve[first] * 2 - curve[first * 2 - i], 0), 255);
                                    sum[i] = curve[i];
                                    div[i] = 1;
                                }
                            }
                        }
                    }

                    if (div[255] == 0) {
                        int last = -1;
                        for (int l = 255; l >= 0; l--) {
                            if (div[l] != 0) {
                                last = l;
                                break;
                            }
                        }

                        // Extend top
                        for (int i = 255; i > last; i--) {
                            if (last * 2 - i >= 0) {
                                if (div[last * 2 - i] != 0) {
                                    curve[i] = std::min(std::max(curve[last] * 2 - curve[last * 2 - i], 0), 255);
                                    sum[i] = curve[i];
                                    div[i] = 1;
                                }
                            }
                        }
                    }
                }

                // Smooth curve
                if (smoothing_window > 0) {
                    for (int i = 0; i < 256; i++) {
                        sum[i] = 0;
                        div[i] = 0;

                        for (int j = -smoothing_window; j < +smoothing_window; j++) {
                            if (i + j >= 0 && i + j < 256) {
                                sum[i] += curve[i + j];
                                div[i] += 1;
                            }
                        }
                    }
                }

                for (int i = 0; i < 256; i++) {
                    curve[i] = IntDiv(sum[i], div[i]);
                }
            }
        }
    }

    void Process(const uint8_t* srcp, uint8_t* dstp, int width, int height, int stride, int dst_stride) {
        for (int h = 0; h < height; h++) {
            for (int w = 0; w < width; w++)
                dstp[w] = curve[srcp[w]];

            srcp += stride;
            dstp += dst_stride;
        }
    }

    void Show(uint8_t* ptr, int stride, uint8_t color) {
        for (int i = 0; i < 256; i++)
            ptr[((255 - curve[i]) * stride) + i] = color;
    }

    void Debug(uint8_t* ptr, int stride) {
        for (int i = 0; i < 256; i++) {
            for (int j = 0; j <= curve[i]; j++) {
                ptr[((255 - j) * stride) + i] = curve[i];
            }
        }

        for (int i = 0; i < 256; i++) {
            if (curve[i] > 0) {
                ptr[((255 - curve[i]) * stride) + i] = 255;
            }
        }
    }
};

static void copy_plane(PVideoFrame& dst, PVideoFrame& src, int plane, IScriptEnvironment* env)
{
    const uint8_t* srcp = src->GetReadPtr(plane);
    int src_pitch = src->GetPitch(plane);
    int height = src->GetHeight(plane);
    int row_size = src->GetRowSize(plane);
    uint8_t* destp = dst->GetWritePtr(plane);
    int dst_pitch = dst->GetPitch(plane);
    env->BitBlt(destp, dst_pitch, srcp, src_pitch, row_size, height);
}

class MatchHistogram : public GenericVideoFilter {
    PClip clip;
    PClip clip1;

    bool _raw;
    bool _show;
    bool _debug;
    int _smoothing_window;
    bool _y, _u, _v;
    bool processPlane[3];
    bool has_at_least_v8;

public:
    MatchHistogram(PClip _child, PClip _clip, PClip _clip1, bool raw, bool show, bool debug, int smoothing_window, bool y, bool u, bool v, IScriptEnvironment* env)
        : GenericVideoFilter(_child), clip(_clip), clip1(_clip1), _raw(raw), _show(show), _debug(debug), _smoothing_window(smoothing_window), _y(y), _u(u), _v(v)
    {
        has_at_least_v8 = true;
        try { env->CheckVersion(8); } catch (const AvisynthError&) { has_at_least_v8 = false; }

        const VideoInfo &vi2 = clip->GetVideoInfo();
        const VideoInfo &vi3 = clip1->GetVideoInfo();

        if (!vi.IsSameColorspace(vi2) || !vi.IsSameColorspace(vi3))
            env->ThrowError("MatchHistogram: the clips must have the same colorspace.");

        if (vi.width != vi2.width || vi.height != vi2.height)
            env->ThrowError("MatchHistogram: the first two clips must have the same dimensions.");

        if (vi.width == 0 || vi.height == 0 || vi3.width == 0 || vi3.height == 0)
            env->ThrowError("MatchHistogram: the clips must have constant format and dimensions.");

        if (vi.IsRGB() || vi.BitsPerComponent() > 8)
            env->ThrowError("MatchHistogram: the clips must have 8 bits per sample and must not be RGB.");

        if (_show && (vi.width < 256 || vi.height < 256 || vi3.width < 256 || vi3.height < 256))
            env->ThrowError("MatchHistogram: clips must be at least 256x256 pixels when show is True.");

        if (_debug)
        {
            if (vi.NumComponents() > 1)
                env->ThrowError("MatchHistogram: only one plane can be processed at a time when debug is True.");

            vi.width = 256;
            vi.height = 256;
        }
        else
            vi = vi3;


        int planecount = min(vi.NumComponents(), 3);
        for (int i = 0; i < planecount; i++)
        {
            if (i == 0)
                processPlane[i] = _y;
            else if (i == 1)
                processPlane[i] = _u;
            else
                processPlane[i] = _v;
        }
    }

    PVideoFrame __stdcall GetFrame(int n, IScriptEnvironment* env)
    {
        PVideoFrame src1 = child->GetFrame(n, env);
        PVideoFrame src2 = clip->GetFrame(n, env);
        PVideoFrame src3 = clip1->GetFrame(n, env);
        PVideoFrame dst;
        if (has_at_least_v8) dst = env->NewVideoFrameP(vi, &src1); else dst = env->NewVideoFrame(vi);

        CurveData curve;

        int planes_y[4] = { PLANAR_Y, PLANAR_U, PLANAR_V, PLANAR_A };
        int planecount = min(vi.NumComponents(), 3);
        for (int i = 0; i < planecount; i++)
        {
            const int plane = planes_y[i];         
            
            int src_stride = src1->GetPitch(plane);
            int src1_stride = src2->GetPitch(plane);
            int src3dst_width = src3->GetRowSize(plane);
            int src_width = src1->GetRowSize(plane);
            int dst_width = dst->GetRowSize(plane);
            int src_height = src1->GetHeight(plane);
            int src3dst_height = src3->GetHeight(plane);
            int dst_stride = dst->GetPitch(plane);            
            int dst_height = dst->GetHeight(plane);          
            const uint8_t* src1p = src1->GetReadPtr(plane);
            const uint8_t* src2p = src2->GetReadPtr(plane);
            const uint8_t* src3p = src3->GetReadPtr(plane);
            uint8_t* dstp = dst->GetWritePtr(plane);

            if (_debug)
            {
                fillPlane(dstp, dst_width, dst_height, dst_stride, i ? 128 : 0);

                if (processPlane[i])
                {
                    curve.Create(src1p, src2p, src_width, src_height, src_stride, src1_stride, _raw, _smoothing_window);
                    curve.Debug(dst->GetWritePtr(PLANAR_Y), dst->GetPitch(PLANAR_Y));
                }
            }
            else
            {
                uint8_t show_colors[3] = { 235, 160, 96 };             

                if (processPlane[i])
                {
                    curve.Create(src1p, src2p, src_width, src_height, src_stride, src1_stride, _raw, _smoothing_window);
                    curve.Process(src3p, dstp, src3dst_width, src3dst_height, src_stride, dst_stride);
                }
                else
                    copy_plane(dst, src3, plane, env);

                if (_show)
                {
                    fillPlane(dstp, 256 >> (i ? vi.GetPlaneWidthSubsampling(plane) : 0), 256 >> (i ? vi.GetPlaneHeightSubsampling(plane) : 0), dst_stride, i ? 128 : 16);

                    if (processPlane[i])
                        curve.Show(dst->GetWritePtr(PLANAR_Y), dst->GetPitch(PLANAR_Y), show_colors[i]);
                }
            }
        }

        return dst;
    }
};

AVSValue __cdecl Create_MatchHistogram(AVSValue args, void* user_data, IScriptEnvironment* env)
{
    return new MatchHistogram(
        args[0].AsClip(),
        args[1].AsClip(),
        args[2].AsClip(),
        args[3].AsBool(false),
        args[4].AsBool(false),
        args[5].AsBool(false),
        args[6].AsInt(8),
        args[7].AsBool(true),
        args[8].AsBool(false),
        args[9].AsBool(false),
        env);
}

const AVS_Linkage* AVS_linkage;

extern "C" __declspec(dllexport)
const char* __stdcall AvisynthPluginInit3(IScriptEnvironment * env, const AVS_Linkage* const vectors)
{
    AVS_linkage = vectors;

    env->AddFunction("MatchHistogram", "ccc[raw]b[show]b[debug]b[smoothing_window]i[y]b[u]b[v]b", Create_MatchHistogram, NULL);
    return "MatchHistogram";
}