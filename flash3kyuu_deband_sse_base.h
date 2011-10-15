#include "flash3kyuu_deband.h"

#include "impl_dispatch.h"

#include "sse_compat.h"

#include "sse_utils.h"

#include "dither_high.h"

#include "x64_compat.h"

#include "debug_dump.h"

/****************************************************************************
 * NOTE: DON'T remove static from any function in this file, it is required *
 *       for generating code in multiple SSE versions.                      *
 ****************************************************************************/

typedef struct _info_cache
{
    int pitch;
    char* data_stream;
} info_cache;

static void destroy_cache(void* data)
{
    assert(data);

    info_cache* cache = (info_cache*) data;
    _aligned_free(cache->data_stream);
    free(data);
}

#define UPDOWNSAMPLING_BIT_SHIFT (INTERNAL_BIT_DEPTH - 8)

static __inline __m128i clamped_absolute_difference(__m128i a, __m128i b, __m128i difference_limit)
{
    // we need to clamp the result for 2 reasons:
    // 1. there is no integer >= operator in SSE
    // 2. comparison instructions accept only signed integers,
    //    so if difference is bigger than 0x7f, the compare result will be invalid
    __m128i diff = _mm_sub_epi8(_mm_max_epu8(a, b), _mm_min_epu8(a, b));
    return _mm_min_epu8(diff, difference_limit);
}

#ifdef ENABLE_DEBUG_DUMP

template<int precision_mode>
static void __forceinline _dump_value_group(const TCHAR* name, __m128i part1, __m128i part2, bool is_signed=false)
{
    if (precision_mode == PRECISION_LOW)
    {
        DUMP_VALUE_S(name, part1, 1, is_signed);
    } else {
        DUMP_VALUE_S(name, part1, 2, is_signed);
        DUMP_VALUE_S(name, part2, 2, is_signed);
    }
}

#define DUMP_VALUE_GROUP(name, ...) _dump_value_group<precision_mode>(TEXT(name), __VA_ARGS__)

#else

#define DUMP_VALUE_GROUP(name, ...) ((void)0)

#endif


template <int sample_mode, int ref_part_index>
static __forceinline void process_plane_info_block(
    pixel_dither_info *&info_ptr, 
    const unsigned char* src_addr_start, 
    const __m128i &src_pitch_vector, 
    __m128i &change_1, 
    __m128i &change_2, 
    const __m128i &minus_one, 
    const __m128i &width_subsample_vector,
    const __m128i &height_subsample_vector,
    const __m128i &pixel_step_shift_bits,
    char*& info_data_stream)
{
    __m128i info_block = _mm_load_si128((__m128i*)info_ptr);

    if (sample_mode > 0) {
        // change: bit 16-31
        __m128i change_temp;
        change_temp = info_block;
        change_temp = _mm_srai_epi32(change_temp, 16);

        switch (ref_part_index)
        {
        case 0:
            change_1 = change_temp;
            break;
        case 1:
            change_1 = _mm_packs_epi32(change_1, change_temp);
            break;
        case 2:
            change_2 = change_temp;
            break;
        case 3:
            change_2 = _mm_packs_epi32(change_2, change_temp);
            break;
        }
    
    }

    // ref1: bit 0-7
    // left-shift & right-shift 24bits to remove other elements and preserve sign
    __m128i ref1 = info_block;
    ref1 = _mm_slli_epi32(ref1, 24); // << 24
    ref1 = _mm_srai_epi32(ref1, 24); // >> 24

    DUMP_VALUE("ref1", ref1, 4, true);

    __m128i ref_offset1;
    __m128i ref_offset2;

    __m128i temp_ref1;
    switch (sample_mode)
    {
    case 0:
        // ref1 = (abs(ref1) >> height_subsampling) * (sign(ref1))
        temp_ref1 = _mm_abs_epi32(ref1);
        temp_ref1 = _mm_sra_epi32(temp_ref1, height_subsample_vector);
        temp_ref1 = _cmm_mullo_limit16_epi32(temp_ref1, _mm_srai_epi32(ref1, 31));
        ref_offset1 = _cmm_mullo_limit16_epi32(src_pitch_vector, temp_ref1); // packed DWORD multiplication
        DUMP_VALUE("ref_pos", ref_offset1, 4, true);
        break;
    case 1:
        // ref1 is guarenteed to be postive
        temp_ref1 = _mm_sra_epi32(ref1, height_subsample_vector);
        ref_offset1 = _cmm_mullo_limit16_epi32(src_pitch_vector, temp_ref1); // packed DWORD multiplication
        DUMP_VALUE("ref_pos", ref_offset1, 4, true);

        ref_offset2 = _cmm_negate_all_epi32(ref_offset1, minus_one); // negates all offsets
        break;
    case 2:
        // ref2: bit 8-15
        // similar to above
        __m128i ref2;
        ref2 = info_block;
        ref2 = _mm_slli_epi32(ref2, 16); // << 16
        ref2 = _mm_srai_epi32(ref2, 24); // >> 24

        __m128i ref1_fix, ref2_fix;
        // ref_px = src_pitch * info.ref2 + info.ref1;
        ref1_fix = _mm_sra_epi32(ref1, width_subsample_vector);
        ref2_fix = _mm_sra_epi32(ref2, height_subsample_vector);
        ref_offset1 = _cmm_mullo_limit16_epi32(src_pitch_vector, ref2_fix); // packed DWORD multiplication
        ref_offset1 = _mm_add_epi32(ref_offset1, _mm_sll_epi32(ref1_fix, pixel_step_shift_bits));
        DUMP_VALUE("ref_pos", ref_offset1, 4, true);

        // ref_px_2 = info.ref2 - src_pitch * info.ref1;
        ref1_fix = _mm_sra_epi32(ref1, height_subsample_vector);
        ref2_fix = _mm_sra_epi32(ref2, width_subsample_vector);
        ref_offset2 = _cmm_mullo_limit16_epi32(src_pitch_vector, ref1_fix); // packed DWORD multiplication
        ref_offset2 = _mm_sub_epi32(_mm_sll_epi32(ref2_fix, pixel_step_shift_bits), ref_offset2);
        DUMP_VALUE("ref_pos_2", ref_offset2, 4, true);
        break;
    default:
        abort();
    }

    if (info_data_stream){
        _mm_store_si128((__m128i*)info_data_stream, ref_offset1);
        info_data_stream += 16;

        if (sample_mode == 2) {
            _mm_store_si128((__m128i*)info_data_stream, ref_offset2);
            info_data_stream += 16;
        }
    }

    info_ptr += 4;
}


static __m128i __inline process_pixels_mode0(__m128i src_pixels, __m128i threshold_vector, const __m128i& ref_pixels)
{
    __m128i dst_pixels;
    __m128i blend_mask;

    __m128i difference;

    difference = clamped_absolute_difference(src_pixels, ref_pixels, threshold_vector);
    // mask: if difference >= threshold, set to 0xff, otherwise 0x00
    // difference is already clamped to threshold, so we compare for equality here
    blend_mask = _mm_cmpeq_epi8(difference, threshold_vector);

    // if mask is 0xff (over threshold), select second operand, otherwise select first
    dst_pixels = _cmm_blendv_by_cmp_mask_epi8(ref_pixels, src_pixels, blend_mask);

    return dst_pixels;
}

template<int sample_mode, bool blur_first>
static __m128i __inline process_pixels_mode12(
    __m128i src_pixels, 
    __m128i threshold_vector, 
    __m128i sign_convert_vector, 
    const __m128i& one_i8, 
    const __m128i& change, 
    const __m128i& ref_pixels_1, 
    const __m128i& ref_pixels_2, 
    const __m128i& ref_pixels_3, 
    const __m128i& ref_pixels_4,
    const __m128i& clamp_high_add,
    const __m128i& clamp_high_sub,
    const __m128i& clamp_low,
    bool need_clamping)
{
    __m128i dst_pixels;
    __m128i use_orig_pixel_blend_mask;
    __m128i avg;

    __m128i difference;

    if (!blur_first)
    {
        difference = clamped_absolute_difference(src_pixels, ref_pixels_1, threshold_vector);
        // mask: if difference >= threshold, set to 0xff, otherwise 0x00
        // difference is already clamped to threshold, so we compare for equality here
        use_orig_pixel_blend_mask = _mm_cmpeq_epi8(difference, threshold_vector);

        difference = clamped_absolute_difference(src_pixels, ref_pixels_2, threshold_vector);
        // use OR to combine the masks
        use_orig_pixel_blend_mask = _mm_or_si128(_mm_cmpeq_epi8(difference, threshold_vector), use_orig_pixel_blend_mask);
    }

    avg = _mm_avg_epu8(ref_pixels_1, ref_pixels_2);

    if (sample_mode == 2)
    {

        if (!blur_first)
        {
            difference = clamped_absolute_difference(src_pixels, ref_pixels_3, threshold_vector);
            use_orig_pixel_blend_mask = _mm_or_si128(_mm_cmpeq_epi8(difference, threshold_vector), use_orig_pixel_blend_mask);

            difference = clamped_absolute_difference(src_pixels, ref_pixels_4, threshold_vector);
            use_orig_pixel_blend_mask = _mm_or_si128(_mm_cmpeq_epi8(difference, threshold_vector), use_orig_pixel_blend_mask);
        }
        // PAVGB adds 1 before calculating average, so we subtract 1 here to be consistent with c version

        __m128i avg2_tmp = _mm_avg_epu8(ref_pixels_3, ref_pixels_4);
        __m128i avg2 = _mm_min_epu8(avg, avg2_tmp);

        avg = _mm_max_epu8(avg, avg2_tmp);
        avg = _mm_subs_epu8(avg, one_i8);

        avg = _mm_avg_epu8(avg, avg2);

    }

    if (blur_first)
    {
        difference = clamped_absolute_difference(src_pixels, avg, threshold_vector);
        use_orig_pixel_blend_mask = _mm_cmpeq_epi8(difference, threshold_vector);
    }

    // if mask is 0xff (over threshold), select second operand, otherwise select first
    src_pixels = _cmm_blendv_by_cmp_mask_epi8(avg, src_pixels, use_orig_pixel_blend_mask);

    // convert to signed form, since change is signed
    src_pixels = _mm_sub_epi8(src_pixels, sign_convert_vector);

    // saturated add
    src_pixels = _mm_adds_epi8(src_pixels, change);

    // convert back to unsigned
    dst_pixels = _mm_add_epi8(src_pixels, sign_convert_vector);

    if (need_clamping)
    {
        dst_pixels = low_bit_depth_pixels_clamp(dst_pixels, clamp_high_add, clamp_high_sub, clamp_low);
    }
    return dst_pixels;
}

static __forceinline __m128i generate_blend_mask_high(__m128i a, __m128i b, __m128i threshold)
{
    __m128i diff1 = _mm_subs_epu16(a, b);
    __m128i diff2 = _mm_subs_epu16(b, a);

    __m128i abs_diff = _mm_or_si128(diff1, diff2);

    __m128i sign_convert_vector = _mm_set1_epi16( (short)0x8000 );

    __m128i converted_diff = _mm_sub_epi16(abs_diff, sign_convert_vector);

    __m128i converted_threshold = _mm_sub_epi16(threshold, sign_convert_vector);

    // mask: if threshold >= diff, set to 0xff, otherwise 0x00
    // note that this is the opposite of low bitdepth implementation
    return _mm_cmpgt_epi16(converted_threshold, converted_diff);
}


template<int sample_mode, bool blur_first>
static __m128i __forceinline process_pixels_mode12_high_part(__m128i src_pixels, __m128i threshold_vector, __m128i change, const __m128i& ref_pixels_1, const __m128i& ref_pixels_2, const __m128i& ref_pixels_3, const __m128i& ref_pixels_4)
{	
    __m128i use_orig_pixel_blend_mask;
    __m128i avg;

    if (!blur_first)
    {
        use_orig_pixel_blend_mask = generate_blend_mask_high(src_pixels, ref_pixels_1, threshold_vector);

        // note: use AND instead of OR, because two operands are reversed
        // (different from low bit-depth mode!)
        use_orig_pixel_blend_mask = _mm_and_si128(
            use_orig_pixel_blend_mask, 
            generate_blend_mask_high(src_pixels, ref_pixels_2, threshold_vector) );
    }

    avg = _mm_avg_epu16(ref_pixels_1, ref_pixels_2);

    if (sample_mode == 2)
    {
        if (!blur_first)
        {
            use_orig_pixel_blend_mask = _mm_and_si128(
                use_orig_pixel_blend_mask, 
                generate_blend_mask_high(src_pixels, ref_pixels_3, threshold_vector) );

            use_orig_pixel_blend_mask = _mm_and_si128(
                use_orig_pixel_blend_mask, 
                generate_blend_mask_high(src_pixels, ref_pixels_4, threshold_vector) );
        }

        avg = _mm_subs_epu16(avg, _mm_set1_epi16(1));
        avg = _mm_avg_epu16(avg, _mm_avg_epu16(ref_pixels_3, ref_pixels_4));

    }

    if (blur_first)
    {
        use_orig_pixel_blend_mask = generate_blend_mask_high(src_pixels, avg, threshold_vector);
    }
    
    DUMP_VALUE("avg", avg, 2, false);

    // if mask is 0xff (NOT over threshold), select second operand, otherwise select first
    // note this is different from low bitdepth code
    __m128i dst_pixels;

    dst_pixels = _cmm_blendv_by_cmp_mask_epi8(src_pixels, avg, use_orig_pixel_blend_mask);
    
    __m128i sign_convert_vector = _mm_set1_epi16((short)0x8000);

    // convert to signed form, since change is signed
    dst_pixels = _mm_sub_epi16(dst_pixels, sign_convert_vector);

    // saturated add
    dst_pixels = _mm_adds_epi16(dst_pixels, change);

    // convert back to unsigned
    dst_pixels = _mm_add_epi16(dst_pixels, sign_convert_vector);
    return dst_pixels;
}


static __m128i __forceinline high_bit_depth_pixels_shift_to_16bit(__m128i pixels)
{
    if (UPDOWNSAMPLING_BIT_SHIFT < 8)
    {
        pixels = _mm_slli_epi16(pixels, (8 - UPDOWNSAMPLING_BIT_SHIFT));
    }
    return pixels;
}

static __m128i __forceinline high_bit_depth_pixels_shift_to_8bit(__m128i pixels)
{
    return _mm_srli_epi16(pixels, UPDOWNSAMPLING_BIT_SHIFT);
}

template<int sample_mode, bool blur_first, int precision_mode>
static __m128i __forceinline process_pixels_mode12_high(
    __m128i src_pixels_0, 
    __m128i src_pixels_1, 
    __m128i threshold_vector, 
    const __m128i& change_1, 
    const __m128i& change_2, 
    const __m128i& ref_pixels_1_0,
    const __m128i& ref_pixels_1_1,
    const __m128i& ref_pixels_2_0,
    const __m128i& ref_pixels_2_1,
    const __m128i& ref_pixels_3_0,
    const __m128i& ref_pixels_3_1,
    const __m128i& ref_pixels_4_0,
    const __m128i& ref_pixels_4_1,
    const __m128i& clamp_high_add,
    const __m128i& clamp_high_sub,
    const __m128i& clamp_low,
    bool need_clamping,
    int row, 
    int column, 
    int height, 
    int dst_pitch, 
    __m128i* dst_px, 
    void* dither_context)
{
    __m128i zero = _mm_setzero_si128();
    
    __m128i lo = process_pixels_mode12_high_part<sample_mode, blur_first>
        (src_pixels_0, 
         threshold_vector, 
         change_1, 
         ref_pixels_1_0, 
         ref_pixels_2_0, 
         ref_pixels_3_0, 
         ref_pixels_4_0 );

    __m128i hi = process_pixels_mode12_high_part<sample_mode, blur_first>
        (src_pixels_1, 
         threshold_vector, 
         change_2, 
         ref_pixels_1_1, 
         ref_pixels_2_1, 
         ref_pixels_3_1, 
         ref_pixels_4_1 );
    
    DUMP_VALUE_GROUP("new_pixel_before_downsample", lo, hi);

    switch (precision_mode)
    {
    case PRECISION_LOW:
    case PRECISION_HIGH_NO_DITHERING:
    case PRECISION_HIGH_ORDERED_DITHERING:
    case PRECISION_HIGH_FLOYD_STEINBERG_DITHERING:
        {
            lo = dither_high::dither<precision_mode>(dither_context, lo, row, column);
            hi = dither_high::dither<precision_mode>(dither_context, hi, row, column + 8);

            lo = high_bit_depth_pixels_shift_to_8bit(lo);
            hi = high_bit_depth_pixels_shift_to_8bit(hi);

            __m128i ret = _mm_packus_epi16(lo, hi);
            if (need_clamping)
            {
                ret = low_bit_depth_pixels_clamp(ret, clamp_high_add, clamp_high_sub, clamp_low);
            }
            return ret;
            break;
        }
    case PRECISION_16BIT_STACKED:
        {
            if (need_clamping)
            {
                lo = high_bit_depth_pixels_clamp(lo, clamp_high_add, clamp_high_sub, clamp_low);
                hi = high_bit_depth_pixels_clamp(hi, clamp_high_add, clamp_high_sub, clamp_low);
            }

            __m128i msb_lo = high_bit_depth_pixels_shift_to_8bit(lo);
            __m128i msb_hi = high_bit_depth_pixels_shift_to_8bit(hi);

            __m128i msb = _mm_packus_epi16(msb_lo, msb_hi);
            _mm_store_si128(dst_px, msb);

            __m128i mask = _mm_set1_epi16(0x00ff);
            __m128i lsb_lo;
            __m128i lsb_hi;

            lsb_lo = high_bit_depth_pixels_shift_to_16bit(lo);
            lsb_hi = high_bit_depth_pixels_shift_to_16bit(hi);

            lsb_lo = _mm_and_si128(lsb_lo, mask);
            lsb_hi = _mm_and_si128(lsb_hi, mask);
        
            __m128i lsb = _mm_packus_epi16(lsb_lo, lsb_hi);
            _mm_store_si128((__m128i*)(((char*)dst_px) + dst_pitch * height), lsb);
        }
        break;
    case PRECISION_16BIT_INTERLEAVED:
        {
            if (need_clamping)
            {
                lo = high_bit_depth_pixels_clamp(lo, clamp_high_add, clamp_high_sub, clamp_low);
                hi = high_bit_depth_pixels_clamp(hi, clamp_high_add, clamp_high_sub, clamp_low);
            }
            
            lo = high_bit_depth_pixels_shift_to_16bit(lo);
            hi = high_bit_depth_pixels_shift_to_16bit(hi);
        
            _mm_store_si128(dst_px, lo);
            _mm_store_si128(dst_px + 1, hi);
        }
        break;
    default:
        abort();
    }

    return zero;
}

template<int sample_mode, bool blur_first, int precision_mode>
static __m128i __forceinline process_pixels(
    __m128i src_pixels_0, 
    __m128i src_pixels_1, 
    __m128i threshold_vector, 
    const __m128i& sign_convert_vector, 
    const __m128i& one_i8, 
    const __m128i& change_1, 
    const __m128i& change_2, 
    const __m128i& ref_pixels_1_0,
    const __m128i& ref_pixels_1_1,
    const __m128i& ref_pixels_2_0,
    const __m128i& ref_pixels_2_1,
    const __m128i& ref_pixels_3_0,
    const __m128i& ref_pixels_3_1,
    const __m128i& ref_pixels_4_0,
    const __m128i& ref_pixels_4_1,
    const __m128i& clamp_high_add,
    const __m128i& clamp_high_sub,
    const __m128i& clamp_low,
    bool need_clamping,
    int row, 
    int column, 
    int height, 
    int dst_pitch, 
    __m128i* dst_px, 
    void* dither_context)
{
    switch (sample_mode)
    {
    case 0:
        return process_pixels_mode0(src_pixels_0, threshold_vector, ref_pixels_1_0);
        break;
    case 1:
    case 2:
        if (precision_mode == PRECISION_LOW)
        {
            return process_pixels_mode12<sample_mode, blur_first>(
                       src_pixels_0, 
                       threshold_vector, 
                       sign_convert_vector, 
                       one_i8, 
                       change_1, 
                       ref_pixels_1_0, 
                       ref_pixels_2_0, 
                       ref_pixels_3_0, 
                       ref_pixels_4_0, 
                       clamp_high_add, 
                       clamp_high_sub, 
                       clamp_low, 
                       need_clamping);
        } else {
            return process_pixels_mode12_high<sample_mode, blur_first, precision_mode>(
                       src_pixels_0, 
                       src_pixels_1, 
                       threshold_vector, 
                       change_1, 
                       change_2, 
                       ref_pixels_1_0, 
                       ref_pixels_1_1, 
                       ref_pixels_2_0, 
                       ref_pixels_2_1, 
                       ref_pixels_3_0, 
                       ref_pixels_3_1, 
                       ref_pixels_4_0, 
                       ref_pixels_4_1, 
                       clamp_high_add, 
                       clamp_high_sub, 
                       clamp_low, 
                       need_clamping, 
                       row, 
                       column, 
                       height, 
                       dst_pitch, 
                       dst_px, 
                       dither_context);
        }
        break;
    default:
        abort();
    }
    return _mm_setzero_si128();
}

template<bool aligned>
static __m128i load_m128(const unsigned char *ptr)
{
    if (aligned)
    {
        return _mm_load_si128((const __m128i*)ptr);
    } else {
        return _mm_loadu_si128((const __m128i*)ptr);
    }
}

template<int precision_mode, bool aligned>
static void __forceinline read_pixels(
    const process_plane_params& params,
    const unsigned char *ptr, 
    __m128i upsample_shift,
    __m128i& pixels_1, 
    __m128i& pixels_2)
{
    if (precision_mode == PRECISION_LOW)
    {
        pixels_1 = load_m128<aligned>(ptr);
        return;
    }
    __m128i p1;
    p1 = load_m128<aligned>(ptr);

    switch (params.input_mode)
    {
    case LOW_BIT_DEPTH:
        {
            __m128i zero = _mm_setzero_si128();
            pixels_1 = _mm_unpacklo_epi8(zero, p1);
            pixels_2 = _mm_unpackhi_epi8(zero, p1);
            return;
        }
        break;
    case HIGH_BIT_DEPTH_STACKED:
        {
            __m128i p2 = load_m128<aligned>(ptr + params.plane_height_in_pixels * params.src_pitch);
            pixels_1 = _mm_unpacklo_epi8(p2, p1);
            pixels_2 = _mm_unpackhi_epi8(p2, p1);
        }
        break;
    case HIGH_BIT_DEPTH_INTERLEAVED:
        pixels_1 = p1;
        pixels_2 = load_m128<aligned>(ptr + 16);
        break;
    default:
        abort();
    }
    pixels_1 = _mm_sll_epi16(pixels_1, upsample_shift);
    pixels_2 = _mm_sll_epi16(pixels_2, upsample_shift);
}

template<int precision_mode, INPUT_MODE input_mode>
static unsigned short read_pixel(
    int plane_height_in_pixels,
    int src_pitch,
    const unsigned char* base,
    int offset)
{
    const unsigned char* ptr = base + offset;

    if (precision_mode == PRECISION_LOW)
    {
        return *ptr;
    }

    switch (input_mode)
    {
    case LOW_BIT_DEPTH:
        return *ptr;
        break;
    case HIGH_BIT_DEPTH_STACKED:
        return *ptr << 8 | *(ptr + plane_height_in_pixels * src_pitch);
        break;
    case HIGH_BIT_DEPTH_INTERLEAVED:
        return *(unsigned short*)ptr;
        break;
    default:
        // shouldn't happen!
        abort();
        return 0;
    }

}

template <int precision_mode>
static void __forceinline transfer_reference_pixels(
    __m128i shift,
    const unsigned short src[16],
    __m128i& dst_0,
    __m128i& dst_1)
{
    if (precision_mode == PRECISION_LOW)
    {
        dst_0 = _mm_packus_epi16(_mm_load_si128((const __m128i*)src), _mm_load_si128((const __m128i*)(src + 8)));
    } else {
        dst_0 = _mm_load_si128((const __m128i*)src);
        dst_1 = _mm_load_si128((const __m128i*)(src + 8));
        
        dst_0 = _mm_sll_epi16(dst_0, shift);
        dst_1 = _mm_sll_epi16(dst_1, shift);
    }
}


template<int sample_mode, int precision_mode, INPUT_MODE input_mode>
static void __forceinline read_reference_pixels(
    const process_plane_params& params,
    __m128i shift,
    const unsigned char* src_px_start,
    const char* info_data_start,
    __m128i& ref_pixels_1_0,
    __m128i& ref_pixels_1_1,
    __m128i& ref_pixels_2_0,
    __m128i& ref_pixels_2_1,
    __m128i& ref_pixels_3_0,
    __m128i& ref_pixels_3_1,
    __m128i& ref_pixels_4_0,
    __m128i& ref_pixels_4_1)
{
    __declspec(align(16))
    unsigned short tmp_1[16];
    __declspec(align(16))
    unsigned short tmp_2[16];
    __declspec(align(16))
    unsigned short tmp_3[16];
    __declspec(align(16))
    unsigned short tmp_4[16];

    // cache layout: 16 offset groups (1 or 2 offsets / group depending on sample mode) in a pack, 
    //               followed by 32 bytes of change values
    // in the case of 2 offsets / group, offsets are stored like this:
    // [1 1 1 1 
    //  2 2 2 2
    //  1 1 1 1
    //  2 2 2 2
    //  .. snip
    //  1 1 1 1
    //  2 2 2 2]

    int plane_height_in_pixels = params.plane_height_in_pixels;
    int src_pitch = params.src_pitch;

    int i_fix = 0;
    int i_fix_step = (input_mode != HIGH_BIT_DEPTH_INTERLEAVED ? 1 : 2);
    
    switch (sample_mode)
    {
    case 0:
        for (int i = 0; i < 16; i++)
        {
            tmp_1[i] = read_pixel<precision_mode, input_mode>(plane_height_in_pixels, src_pitch, src_px_start, i_fix + *(int*)(info_data_start + 4 * i));
            i_fix += i_fix_step;
        }
        transfer_reference_pixels<precision_mode>(shift, tmp_1, ref_pixels_1_0, ref_pixels_1_1);
        break;
    case 1:
        for (int i = 0; i < 16; i++)
        {
            tmp_1[i] = read_pixel<precision_mode, input_mode>(plane_height_in_pixels, src_pitch, src_px_start, i_fix + *(int*)(info_data_start + 4 * i));
            tmp_2[i] = read_pixel<precision_mode, input_mode>(plane_height_in_pixels, src_pitch, src_px_start, i_fix + -*(int*)(info_data_start + 4 * i));
            i_fix += i_fix_step;
        }
        transfer_reference_pixels<precision_mode>(shift, tmp_1, ref_pixels_1_0, ref_pixels_1_1);
        transfer_reference_pixels<precision_mode>(shift, tmp_2, ref_pixels_2_0, ref_pixels_2_1);
        break;
    case 2:
        for (int i = 0; i < 16; i++)
        {
            tmp_1[i] = read_pixel<precision_mode, input_mode>(plane_height_in_pixels, src_pitch, src_px_start, i_fix + *(int*)(info_data_start + 4 * (i + i / 4 * 4)));
            tmp_2[i] = read_pixel<precision_mode, input_mode>(plane_height_in_pixels, src_pitch, src_px_start, i_fix + *(int*)(info_data_start + 4 * (i + i / 4 * 4 + 4)));
            tmp_3[i] = read_pixel<precision_mode, input_mode>(plane_height_in_pixels, src_pitch, src_px_start, i_fix + -*(int*)(info_data_start + 4 * (i + i / 4 * 4)));
            tmp_4[i] = read_pixel<precision_mode, input_mode>(plane_height_in_pixels, src_pitch, src_px_start, i_fix + -*(int*)(info_data_start + 4 * (i + i / 4 * 4 + 4)));
            i_fix += i_fix_step;
        }
        transfer_reference_pixels<precision_mode>(shift, tmp_1, ref_pixels_1_0, ref_pixels_1_1);
        transfer_reference_pixels<precision_mode>(shift, tmp_2, ref_pixels_2_0, ref_pixels_2_1);
        transfer_reference_pixels<precision_mode>(shift, tmp_3, ref_pixels_3_0, ref_pixels_3_1);
        transfer_reference_pixels<precision_mode>(shift, tmp_4, ref_pixels_4_0, ref_pixels_4_1);
        break;
    }
}


template<int sample_mode, bool blur_first, int precision_mode, bool aligned>
static void __cdecl _process_plane_sse_impl(const process_plane_params& params, process_plane_context* context)
{

    DUMP_INIT("sse", params.plane, params.plane_width_in_pixels);

    pixel_dither_info* info_ptr = params.info_ptr_base;

    __m128i src_pitch_vector = _mm_set1_epi32(params.src_pitch);
           
    __m128i threshold_vector;
    
    if (precision_mode == PRECISION_LOW || sample_mode == 0)
    {
        threshold_vector = _mm_set1_epi8((unsigned char)params.threshold);
    } else {
        threshold_vector = _mm_set1_epi16(params.threshold);
    }

    __m128i sign_convert_vector = _mm_set1_epi8(0x80u);

    // general-purpose constant
    __m128i minus_one = _mm_set1_epi32(-1);

    __m128i one_i8 = _mm_set1_epi8(1);
    
    bool use_cached_info = false;

    char* info_data_stream = NULL;

    char context_buffer[DITHER_CONTEXT_BUFFER_SIZE];

    dither_high::init<precision_mode>(context_buffer, params.plane_width_in_pixels);

    info_cache *cache = NULL;
    
    __m128i width_subsample_vector = _mm_set_epi32(0, 0, 0, params.width_subsampling);
    __m128i height_subsample_vector = _mm_set_epi32(0, 0, 0, params.height_subsampling);

    bool need_clamping = (INTERNAL_BIT_DEPTH < 16 && precision_mode != PRECISION_LOW) || 
                          params.pixel_min > 0 || 
                          params.pixel_max < 0xffff;
    __m128i clamp_high_add = _mm_setzero_si128();
    __m128i clamp_high_sub = _mm_setzero_si128();
    __m128i clamp_low = _mm_setzero_si128();
    if (need_clamping)
    {
        clamp_low = _mm_set1_epi16((short)params.pixel_min);
        clamp_high_add = _mm_sub_epi16(_mm_set1_epi16((short)0xffff), _mm_set1_epi16((short)params.pixel_max));
        clamp_high_sub = _mm_add_epi16(clamp_high_add, clamp_low);
        if (precision_mode < PRECISION_16BIT_STACKED)
        {
            #define CONVERT_TO_8BIT_VALUE(x) { \
                x = _mm_srli_epi16(x, UPDOWNSAMPLING_BIT_SHIFT); \
                x = _mm_packus_epi16(x, x); \
            }

            CONVERT_TO_8BIT_VALUE(clamp_low);
            CONVERT_TO_8BIT_VALUE(clamp_high_add);
            CONVERT_TO_8BIT_VALUE(clamp_high_sub);

            #undef CONVERT_TO_8BIT_VALUE
        }
    }
    
    __m128i pixel_step_shift_bits;
    __m128i upsample_to_16_shift_bits;

    if (params.input_mode == HIGH_BIT_DEPTH_INTERLEAVED)
    {
        pixel_step_shift_bits = _mm_set_epi32(0, 0, 0, 1);
    } else {
        pixel_step_shift_bits = _mm_setzero_si128();
    }
    upsample_to_16_shift_bits = _mm_set_epi32(0, 0, 0, 16 - params.input_depth);

    __declspec(align(16))
    char dummy_info_buffer[128];

    // initialize storage for pre-calculated pixel offsets
    if (context->data) {
        cache = (info_cache*) context->data;
        // we need to ensure src_pitch is the same, otherwise offsets will be completely wrong
        // also, if pitch changes, don't waste time to update the cache since it is likely to change again
        if (cache->pitch == params.src_pitch) {
            info_data_stream = cache->data_stream;
            use_cached_info = true;
        }
    } else {
        // set up buffer for cache
        cache = (info_cache*)malloc(sizeof(info_cache));
        // 4 offsets (2 bytes per item) + 2-byte change
        info_data_stream = (char*)_aligned_malloc(params.info_stride * (4 * 2 + 2) * params.src_height, FRAME_LUT_ALIGNMENT);
        cache->data_stream = info_data_stream;
        cache->pitch = params.src_pitch;
    }

    int info_cache_block_size = (sample_mode == 2 ? 128 : 64);

    int input_mode = params.input_mode;

    for (int row = 0; row < params.plane_height_in_pixels; row++)
    {
        const unsigned char* src_px = params.src_plane_ptr + params.src_pitch * row;
        unsigned char* dst_px = params.dst_plane_ptr + params.dst_pitch * row;

        // info_ptr = info_ptr_base + info_stride * row;
        // doesn't need here, since info_stride equals to count of pixels that are needed to process in each row

        int processed_pixels = 0;

        while (processed_pixels < params.plane_width_in_pixels)
        {
            __m128i change_1, change_2;
            
            __m128i ref_pixels_1_0;
            __m128i ref_pixels_1_1;
            __m128i ref_pixels_2_0;
            __m128i ref_pixels_2_1;
            __m128i ref_pixels_3_0;
            __m128i ref_pixels_3_1;
            __m128i ref_pixels_4_0;
            __m128i ref_pixels_4_1;

#define READ_REFS(data_stream, inp_mode) read_reference_pixels<sample_mode, precision_mode, inp_mode>( \
                    params, \
                    upsample_to_16_shift_bits, \
                    src_px, \
                    data_stream, \
                    ref_pixels_1_0, \
                    ref_pixels_1_1, \
                    ref_pixels_2_0, \
                    ref_pixels_2_1, \
                    ref_pixels_3_0, \
                    ref_pixels_3_1, \
                    ref_pixels_4_0, \
                    ref_pixels_4_1)

            char * data_stream_block_start;

            if (LIKELY(use_cached_info)) {
                data_stream_block_start = info_data_stream;
                info_data_stream += info_cache_block_size;
                if (sample_mode > 0) {
                    change_1 = _mm_load_si128((__m128i*)info_data_stream);
                    info_data_stream += 16;
                    if (precision_mode != PRECISION_LOW)
                    {
                        change_2 = _mm_load_si128((__m128i*)info_data_stream);
                        info_data_stream += 16;
                    }
                }
            } else {
                // we need to process the info block
                change_1 = _mm_setzero_si128();
                change_2 = _mm_setzero_si128();

                char * data_stream_ptr = info_data_stream;
                if (!data_stream_ptr)
                {
                    data_stream_ptr = dummy_info_buffer;
                }

                data_stream_block_start = data_stream_ptr;
            
    #define PROCESS_INFO_BLOCK(n) \
                process_plane_info_block<sample_mode, n>(info_ptr, src_px, src_pitch_vector, change_1, change_2, minus_one, width_subsample_vector, height_subsample_vector, pixel_step_shift_bits, data_stream_ptr);
            
                PROCESS_INFO_BLOCK(0);
                PROCESS_INFO_BLOCK(1);
                PROCESS_INFO_BLOCK(2);
                PROCESS_INFO_BLOCK(3);

    #undef PROCESS_INFO_BLOCK


                if (precision_mode == PRECISION_LOW)
                {
                    change_1 = _mm_packs_epi16(change_1, change_2);
                }
                
                if (info_data_stream) {
                    info_data_stream += info_cache_block_size;
                    assert(info_data_stream == data_stream_ptr);
                }

                if (sample_mode > 0) {

                    if (info_data_stream) {

                        _mm_store_si128((__m128i*)info_data_stream, change_1);
                        info_data_stream += 16;
                        if (precision_mode != PRECISION_LOW)
                        {
                            _mm_store_si128((__m128i*)info_data_stream, change_2);
                            info_data_stream += 16;
                        }
                    }
                }
            }

            switch (input_mode)
            {
            case LOW_BIT_DEPTH:
                READ_REFS(data_stream_block_start, LOW_BIT_DEPTH);
                break;
            case HIGH_BIT_DEPTH_INTERLEAVED:
                READ_REFS(data_stream_block_start, HIGH_BIT_DEPTH_INTERLEAVED);
                break;
            case HIGH_BIT_DEPTH_STACKED:
                READ_REFS(data_stream_block_start, HIGH_BIT_DEPTH_STACKED);
                break;
            }
            
            
            DUMP_VALUE_GROUP("change", change_1, change_2, true);
            DUMP_VALUE_GROUP("ref_1_up", ref_pixels_1_0, ref_pixels_1_1);
            DUMP_VALUE_GROUP("ref_2_up", ref_pixels_2_0, ref_pixels_2_1);
            DUMP_VALUE_GROUP("ref_3_up", ref_pixels_3_0, ref_pixels_3_1);
            DUMP_VALUE_GROUP("ref_4_up", ref_pixels_4_0, ref_pixels_4_1);

            __m128i src_pixels_0, src_pixels_1;
            // abuse the guard bytes on the end of frame, as long as they are present there won't be segfault
            // garbage data is not an problem
            read_pixels<precision_mode, aligned>(params, src_px, upsample_to_16_shift_bits, src_pixels_0, src_pixels_1);
            DUMP_VALUE_GROUP("src_px_up", src_pixels_0, src_pixels_1);

            __m128i dst_pixels = process_pixels<sample_mode, blur_first, precision_mode>(
                                     src_pixels_0, 
                                     src_pixels_1, 
                                     threshold_vector, 
                                     sign_convert_vector, 
                                     one_i8, 
                                     change_1, 
                                     change_2, 
                                     ref_pixels_1_0, 
                                     ref_pixels_1_1, 
                                     ref_pixels_2_0, 
                                     ref_pixels_2_1, 
                                     ref_pixels_3_0, 
                                     ref_pixels_3_1, 
                                     ref_pixels_4_0, 
                                     ref_pixels_4_1, 
                                     clamp_high_add, 
                                     clamp_high_sub, 
                                     clamp_low, 
                                     need_clamping, 
                                     row, 
                                     processed_pixels, 
                                     params.plane_height_in_pixels, 
                                     params.dst_pitch, 
                                     (__m128i*)dst_px, 
                                     context_buffer);

            
            switch (precision_mode)
            {
            case PRECISION_LOW:
            case PRECISION_HIGH_NO_DITHERING:
            case PRECISION_HIGH_ORDERED_DITHERING:
            case PRECISION_HIGH_FLOYD_STEINBERG_DITHERING:
                _mm_store_si128((__m128i*)dst_px, dst_pixels);
                dst_px += 16;
                break;
            case PRECISION_16BIT_STACKED:
                // already written in process_pixels_mode12_high
                dst_px += 16;
                break;
            case PRECISION_16BIT_INTERLEAVED:
                // same as above
                dst_px += 32;
                break;
            default:
                abort();
            }

            processed_pixels += 16;
            src_px += params.input_mode != HIGH_BIT_DEPTH_INTERLEAVED ? 16 : 32;
        }
        DUMP_NEXT_LINE();
        dither_high::next_row<precision_mode>(context_buffer);
    }
    
    dither_high::complete<precision_mode>(context_buffer);

    // for thread-safety, save context after all data is processed
    if (!use_cached_info && !context->data && cache)
    {
        context->destroy = destroy_cache;
        if (InterlockedCompareExchangePointer(&context->data, cache, NULL) != NULL)
        {
            // other thread has completed first, so we can destroy our copy
            destroy_cache(cache);
        }
    }

    DUMP_FINISH();
}


template<int sample_mode, bool blur_first, int precision_mode>
static void __cdecl process_plane_sse_impl(const process_plane_params& params, process_plane_context* context)
{
    if ( ( (POINTER_INT)params.src_plane_ptr & (PLANE_ALIGNMENT - 1) ) == 0 && (params.src_pitch & (PLANE_ALIGNMENT - 1) ) == 0 )
    {
        _process_plane_sse_impl<sample_mode, blur_first, precision_mode, true>(params, context);
    } else {
        _process_plane_sse_impl<sample_mode, blur_first, precision_mode, false>(params, context);
    }
}