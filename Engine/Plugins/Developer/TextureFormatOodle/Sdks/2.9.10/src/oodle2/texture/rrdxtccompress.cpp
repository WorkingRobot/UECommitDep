// Copyright Epic Games, Inc. All Rights Reserved.
// This source file is licensed solely to users who have
// accepted a valid Unreal Engine license agreement 
// (see e.g., https://www.unrealengine.com/eula), and use
// of this source file is governed by such agreement.

// @cdep pre $cbtargetsse4

#include "rrdxtccompress.h"
#include "rrdxtcblock.h"
#include "rrcolorvecc.h"
#include "rrrand.h"
#include <float.h>
#include "rrdxtccompress.inl"
#include "rrmat3.h"
#include "rrlogutil.h"
#include "bc67tables.h" // single-color fit tables
#include "vec128.inl"

#include "templates/rralgorithm.h"
#include "templates/rrvector_s.h"

#include "rrsimpleprof.h"
//#include "rrsimpleprofstub.h"

//===============================================================================

RR_NAMESPACE_START

//===============================================================================

#define PCA_POWER_ITERS 8

template<typename T> inline T rrSqr(T a) { return a*a; }
inline U8 rrRoundAndClampU8(float f) { return (U8) RR_CLAMP_U8( lrintf(f) ); }
static rrVec3f rrNormalize(const rrVec3f & v)
{
	// NOTE(fg): our SIMD versions use sum_across which sums with 2-away, then with 1-away,
	// so calculation order must be this:
	float len_sq = (rrSqr(v.x) + rrSqr(v.z)) + rrSqr(v.y);
	if (len_sq > 0.0f)
	{
		F32 len = sqrtf(len_sq); // NOTE(fg): divide not mul by recip so we match SSE version exactly
		return rrVec3f(v.x / len, v.y / len, v.z / len);
	}
	else
	{
		return rrVec3f(0.f, 0.f, 0.f);
	}
}

//===============================================================================

// Various AddEndPoints helpers!

// The SingleColor_Compact funcs use tables for optimum single-color fit that also
// constrain max distance between quantized endpoint values to avoid running
// afoul of allowed BC1 decoder tolerances.

rrDXT1UnpackedEndPoints * AddEndPoints_SingleColor_Compact_4C(rrDXT1UnpackedEndPoints * pEndPoints, const rrColor32BGRA & c)
{
	rrDXT1UnpackedEndPoints ep;

	const BC7OptimalEndpoint & opt_r = bc1_optimal_4c[0][c.u.r];
	const BC7OptimalEndpoint & opt_g = bc1_optimal_4c[1][c.u.g];
	const BC7OptimalEndpoint & opt_b = bc1_optimal_4c[0][c.u.b];

	ep.c[0] = rrColorUnpacked565(opt_r.lo, opt_g.lo, opt_b.lo);
	ep.c[1] = rrColorUnpacked565(opt_r.hi, opt_g.hi, opt_b.hi);

	// note : bco.c0.w == bco.c1.w is totally possible
	//	(happens for example when the color is 0 or 255)
	// that's a degenerate block that uses 3-color mode!
	// indices 0xAAA (= 1/2 interp) works fine for that too, so leave it

	// Put in 4-color order if possible
	if (ep.c[0] < ep.c[1])
		RR_NAMESPACE::swap(ep.c[0], ep.c[1]);

	*pEndPoints++ = ep;
	return pEndPoints;
}

rrDXT1UnpackedEndPoints * AddEndPoints_SingleColor_Compact_3C(rrDXT1UnpackedEndPoints * pEndPoints, const rrColor32BGRA & c)
{
	rrDXT1UnpackedEndPoints ep;

	const BC7OptimalEndpoint & opt_r = bc1_optimal_3c[0][c.u.r];
	const BC7OptimalEndpoint & opt_g = bc1_optimal_3c[1][c.u.g];
	const BC7OptimalEndpoint & opt_b = bc1_optimal_3c[0][c.u.b];

	ep.c[0] = rrColorUnpacked565(opt_r.lo, opt_g.lo, opt_b.lo);
	ep.c[1] = rrColorUnpacked565(opt_r.hi, opt_g.hi, opt_b.hi);

	// Put in 3-color order
	if (ep.c[0] > ep.c[1])
		RR_NAMESPACE::swap(ep.c[0].dw, ep.c[1].dw);

	*pEndPoints++ = ep;
	return pEndPoints;
}

rrDXT1UnpackedEndPoints * AddEndPoints_Default(rrDXT1UnpackedEndPoints * pEndPoints, rrDXT1PaletteMode mode,
							const rrColor32BGRA & end1,const rrColor32BGRA & end2)
{
	rrDXT1UnpackedEndPoints ep;
	ep.c[0] = rrColorUnpacked565::quantize(end1);
	ep.c[1] = rrColorUnpacked565::quantize(end2);

	// We check for actual solid colors during block init, but we can
	// still have endpoints close enough that they quantize to the same
	// 565 values:
	if ( ep.c[0] == ep.c[1] )
	{
		// We used to try various tricks here to separate the endpoints slightly,
		// but it was pretty hit and miss and basically nothing actually seems to care
		// much; at least at the higher levels (which are what Oodle Texture actually
		// uses) we also have the "Greedy Optimize" phase at the end that tries +-1
		// wiggles on the endpoint values anyway, which pushes us towards not really
		// caring here.
		//
		// The main texture in our test sets that gets worse from this is linear_ramp1,
		// and only at rrDXT levels 0 and 1 (0 isn't even exposed in Oodle Texture).
		return pEndPoints;
	}

	// Try both 3-color and 4-color orderings
	*pEndPoints++ = ep;

	// Add swapped order if not in four-color mode where both are equivalent
	if ( mode != rrDXT1PaletteMode_FourColor )
	{
		pEndPoints->c[0] = ep.c[1];
		pEndPoints->c[1] = ep.c[0];
		pEndPoints++;
	}

	return pEndPoints;
}

rrDXT1UnpackedEndPoints * AddEndPoints_Force3C(rrDXT1UnpackedEndPoints * pEndPoints,
							const rrColor32BGRA & end1,const rrColor32BGRA & end2)
{
	pEndPoints->c[0] = rrColorUnpacked565::quantize(end1);
	pEndPoints->c[1] = rrColorUnpacked565::quantize(end2);
	// force 3 color order
	if ( pEndPoints->c[0] > pEndPoints->c[1] )
		RR_NAMESPACE::swap(pEndPoints->c[0], pEndPoints->c[1]);
	pEndPoints++;

	return pEndPoints;
}

rrDXT1UnpackedEndPoints * AddEndPoints_BothWays(rrDXT1UnpackedEndPoints * pEndPoints,
							const rrColor32BGRA & end1,const rrColor32BGRA & end2)
{
	rrDXT1UnpackedEndPoints ep;
	ep.c[0] = rrColorUnpacked565::quantize(end1);
	ep.c[1] = rrColorUnpacked565::quantize(end2);
	if ( ep.c[0] == ep.c[1] ) // degenerate, just skip!
		return pEndPoints;
	*pEndPoints++ = ep;

	// Other-direction pair as well (swapped)
	pEndPoints->c[0] = ep.c[1];
	pEndPoints->c[1] = ep.c[0];
	pEndPoints++;

	return pEndPoints;
}

rrDXT1UnpackedEndPoints * AddEndPoints_TwoColorBest(rrDXT1UnpackedEndPoints * pEndPoints,
							const rrColor32BGRA & c1,const rrColor32BGRA & c2)
{
	rrDXT1UnpackedEndPoints * ptr = pEndPoints;

	// this is wasteful, fix to work directly on colors instead of going through vec3i :
	rrVec3i v1 = ColorToVec3i(c1);
	rrVec3i v2 = ColorToVec3i(c2);
	rrVec3i delta = v2 - v1;

	// try to hit two colors exactly by either
	//	using them as the ends or trying to hit them at the 1/3 or 2/3 points

	// I only actually try 4 ways, I should just unroll them :
	// 0 : c1 , 1 : c2
	// 0 : c1 , 2/3 : c2
	// 1/3 : c1 , 1 : c2
	// 1/3 : c1 , 2/3 : c2

	// these ways are actually not tried now :
	// 2/3 : c1 , 1 : c2
	// 0 : c1 , 1/3 : c2

	//0,3 : v1->v2
	//0,2 : v1->v2+delta2
	//1,2 : v1-delta->v2+delta
	//1,3 : v1-delta2->v2

	ptr = AddEndPoints_BothWays(ptr,c1,c2);

	// toggle just doing endpoints
	//	0.1 rmse win from this

	rrVec3i delta2;
	delta2.x = delta.x / 2;
	delta2.y = delta.y / 2;
	delta2.z = delta.z / 2;

	// tiny len, don't bother :
	if ( LengthSqr(delta2) < 6 )
		return ptr;

	{
		rrColor32BGRA cend1 = Vec3iToColorClamp(v1 - delta);
		rrColor32BGRA cend2 = Vec3iToColorClamp(v2 + delta);

		ptr = AddEndPoints_BothWays(ptr,cend1,cend2);
	}

	{
		rrColor32BGRA cend1 = Vec3iToColorClamp(v1 - delta2);
		rrColor32BGRA cend2 = Vec3iToColorClamp(v2 + delta2);

		ptr = AddEndPoints_BothWays(ptr,cend1,c2);
		ptr = AddEndPoints_BothWays(ptr,c1,cend2);

		#if 0
		ptr = AddEndPoints_BothWays(ptr,cend1,cend2);
		#endif
	}

	return ptr;
}

//===============================================================================

enum
{
	FilterEndpoints_Allow4c = 1 << 0,
	FilterEndpoints_Allow3c = 1 << 1,
	FilterEndpoints_AllowBoth = FilterEndpoints_Allow4c | FilterEndpoints_Allow3c,
};

// Filters endpoints [endpoints,endpoints_end) to only contain the given mode, in-place,
// and returns new end
static rrDXT1UnpackedEndPoints * FilterEndpoints(rrDXT1UnpackedEndPoints * endpoints, rrDXT1UnpackedEndPoints * endpoints_end, U32 allow_which)
{
	U32 mask = allow_which & FilterEndpoints_AllowBoth;

	// Handle trivial cases
	if ( mask == 0 )
		return endpoints; // none pass
	else if ( mask == FilterEndpoints_AllowBoth )
		return endpoints_end; // all pass

	bool want_4color = mask == FilterEndpoints_Allow4c;
	rrDXT1UnpackedEndPoints * new_end = endpoints;
	for ( rrDXT1UnpackedEndPoints * cur = endpoints; cur != endpoints_end; ++cur )
	{
		if ( DXT1_Is4Color(*cur,rrDXT1PaletteMode_Alpha) == want_4color )
			*new_end++ = *cur;
	}

	return new_end;
}

// Tries a number of endpoint pairs, given as endpoints[0..count-1].
// palette_scratch needs to have space for count*4 entries; doesn't
// need to be initialized, it just needs to be large enough.
static void TryBatchedEndpoints(CpuDispatchFlags dispatch, rrDXT1Block * pBlock, U32 * pError, const rrColorBlock4x4 & colors, const rrDXT1PaletteMode mode,
	const rrDXT1UnpackedEndPoints * endpoints, int count, rrColor32BGRA palette_scratch[])
{
	RR_ASSERT(count < DXT1_FindErrorsContext::COUNT_LIMIT);

	DXT1_FindErrorsContext ctx;
	ctx.init(dispatch, colors);

	// rank all the candidates
	DXT1_ComputePalette_Batched(endpoints, count, palette_scratch, mode);

	U32 best_err_and_i = ctx.find_best_palette(palette_scratch, count);
	U32 best_err = best_err_and_i >> DXT1_FindErrorsContext::COUNT_SHIFT;

	if ( best_err < *pError ) // we found an improvement!
	{
		U32 best_i = best_err_and_i & (DXT1_FindErrorsContext::COUNT_LIMIT - 1);

		pBlock->c0 = endpoints[best_i].c[0].pack();
		pBlock->c1 = endpoints[best_i].c[1].pack();
		pBlock->indices = DXT1_FindIndices(dispatch,colors,&palette_scratch[best_i * 4],pError);
		RR_ASSERT( *pError == best_err );
	}
}

//================================================

struct rrCompressDXT1_Startup_Data
{
	rrVec3i avg;
	rrVec3i diagonal;
	rrVec3i sum;
	//rrColor32BGRA avgC;
	rrColor32BGRA loC;
	rrColor32BGRA hiC;
	CpuDispatchFlags dispatch;
	rrbool has_any_alpha; // has_any_alpha can only be true when mode == rrDXT1PaletteMode_Alpha
	// if has_any_alpha is on, you cannot use 4c mode, must use 3c mode
};

void DXT1_GreedyOptimizeBlock(const rrCompressDXT1_Startup_Data & data,rrDXT1Block * pBlock,U32 * pError, const rrColorBlock4x4 & colors, rrDXT1PaletteMode mode, bool do_joint_optimization);

void DXT1_AnnealBlock(const rrCompressDXT1_Startup_Data & data,rrDXT1Block * pBlock,U32 * pError, const rrColorBlock4x4 & colors, 
	rrDXT1PaletteMode mode, bool expensive_mode);

// for palette mode,
//  is this color a 3rd-index special color? (transparent/black)
bool rrDXT1_IsTransparentOrBlack(rrDXT1PaletteMode mode,const rrColor32BGRA &color)
{
	if ( mode == rrDXT1PaletteMode_FourColor ) return false;
	else if ( mode == rrDXT1PaletteMode_Alpha ) return rrColor32BGRA_IsOneBitTransparent(color);
	else
	{
		// mode == NoAlpha ; is it black ?
		#define BLACKNESS_DISTANCE	12 // @@ blackness threshold
		return ( color.u.b < BLACKNESS_DISTANCE &&
			color.u.g < BLACKNESS_DISTANCE &&
			color.u.r < BLACKNESS_DISTANCE );
	}
}

bool Compress_TryAllPairs_Heavy(CpuDispatchFlags dispatch,rrDXT1Block * pBlock,U32 * pError, const rrColorBlock4x4 & colors, rrDXT1PaletteMode mode)
{
	// Color565 is U16
	vector_s<Color565,16> uniques;
	uniques.resize(16);
	for(int i=0;i<16;i++)
	{
		uniques[i] = Quantize( colors.colors[i] ).w;
	}
	RR_NAMESPACE::stdsort(uniques.begin(),uniques.end());
	vector_s<Color565,16>::iterator it = RR_NAMESPACE::unique(uniques.begin(),uniques.end());
	uniques.erase( it, uniques.end()  );
	
	int count = uniques.size32();
	
	if ( count == 1 )
	{
		// @@ special case; single color
		return false;
	}
	
	bool ret = false;
	for(int i=0;i<count;i++)
	{
		for(int j=i+1;j<count;j++)
		{
			Color565 c0 = uniques[i];
			Color565 c1 = uniques[j];
			
			rrDXT1Block trial;
			
			trial.c0 = ToUnion(c0);
			trial.c1 = ToUnion(c1);	
			
			rrColor32BGRA palette[4];
			U32 err;

			{
				DXT1_ComputePalette(trial.c0,trial.c1,palette,mode);

				trial.indices = DXT1_FindIndices(dispatch,colors,palette,&err);
				
				DXT1_OptimizeEndPointsFromIndicesIterative(dispatch,&trial,&err,colors,mode);
				
				if ( err < *pError )
				{
					ret = true;
					*pError = err;
					*pBlock = trial;
				}
			}
			
			// reverse colors and try again :
			// no point trying this in force-four-color mode, it doesn't give us any new options
			if ( mode != rrDXT1PaletteMode_FourColor )
			{
				RR_NAMESPACE::swap( trial.c0, trial.c1 );

				DXT1_ComputePalette(trial.c0,trial.c1,palette,mode);

				trial.indices = DXT1_FindIndices(dispatch,colors,palette,&err);
				
				DXT1_OptimizeEndPointsFromIndicesIterative(dispatch,&trial,&err,colors,mode);
				
				if ( err < *pError )
				{
					ret = true;
					*pError = err;
					*pBlock = trial;
				}
			}
		}
	}
	
	return ret;
}

static bool rrCompressDXT1_Startup_Impl(CpuDispatchFlags dispatch, rrCompressDXT1_Startup_Data * pData, const rrColorBlock4x4 & colors, rrDXT1PaletteMode mode, rrDXT1UnpackedEndPoints * & endptr)
{
	RR_ASSERT( rrColorBlock4x4_IsBC1Canonical(colors,mode) );

	pData->dispatch = dispatch;

	rrVec3i avg(0,0,0);
	
	rrColor32BGRA loC;
	loC.dw = 0xFFFFFFFF;
	loC.u.a = 0;
	rrColor32BGRA hiC;
	hiC.dw = 0;
	
	int num_colors = 0;
	
	rrColor32BGRA loC_colors;
	loC_colors.dw = 0xFFFFFFFF;
	loC_colors.u.a = 0;
	rrColor32BGRA hiC_colors;
	hiC_colors.dw = 0;
	
	int num_transparent = 0;

	for(int i=0;i<16;i++)
	{
		const rrColor32BGRA & c = colors.colors[i];
		// c is canonical (see assert on entry)

		if ( c.dw == 0 )
		{
			num_transparent++;
		}

		avg += ColorToVec3i( c );
		
		hiC.u.b = RR_MAX(hiC.u.b,c.u.b);
		hiC.u.g = RR_MAX(hiC.u.g,c.u.g);
		hiC.u.r = RR_MAX(hiC.u.r,c.u.r);
		loC.u.b = RR_MIN(loC.u.b,c.u.b);
		loC.u.g = RR_MIN(loC.u.g,c.u.g);
		loC.u.r = RR_MIN(loC.u.r,c.u.r);
		
		if ( ! rrDXT1_IsTransparentOrBlack(mode,c) )
		{
			// if pal_mode == alpha,
			//	then blacks come in here and count as "colors"
			num_colors++;
			
			hiC_colors.u.b = RR_MAX(hiC_colors.u.b,c.u.b);
			hiC_colors.u.g = RR_MAX(hiC_colors.u.g,c.u.g);
			hiC_colors.u.r = RR_MAX(hiC_colors.u.r,c.u.r);
			loC_colors.u.b = RR_MIN(loC_colors.u.b,c.u.b);
			loC_colors.u.g = RR_MIN(loC_colors.u.g,c.u.g);
			loC_colors.u.r = RR_MIN(loC_colors.u.r,c.u.r);
		}
	}
	
	// loC/hiC alphas are all zero

	// hiC includes all colors, degen and non
	if ( hiC.dw == 0 )
	{
		// there can be a mix of opaque-black & transparent here
		// still need indices, but we can definitely use all-black colors

		endptr->c[0].dw = 0; // all-0 puts us in 3c mode, black
		endptr->c[1].dw = 0;
		++endptr;

		return false;
	}

	RR_ASSERT( num_transparent != 16 ); // should have been caught above
	// num_colors == 0 is possible here

	pData->has_any_alpha = num_transparent > 0;

	// "avg" includes all colors, including degens
	rrVec3i sum = avg;
	avg.x = (avg.x + 8)>>4;
	avg.y = (avg.y + 8)>>4;
	avg.z = (avg.z + 8)>>4;
	
	rrColor32BGRA avgC;
	avgC = Vec3iToColor(avg);
	
	if ( ! pData->has_any_alpha )
	{
		// try single color block to get started :
		endptr = AddEndPoints_SingleColor_Compact_4C(endptr,avgC);

		// Try the 3C mode too?
		// NOTE(fg): this helps linear_ramp1 for AMD/Intel but
		// increases error for NV; could pass a flag for us to try this
		// for high-quality modes, but since it seems very linear_ramp
		// specific, not sure?
		/*
		if ( mode != rrDXT1PaletteMode_FourColor )
			endptr = AddEndPoints_SingleColor_Compact_3C(endptr,avgC);
		//*/
	}

	if ( num_colors < 16 )
	{
		if ( num_colors == 0 )
		{
			// degenerate, no colors
			
			// we already checked hiC.dw == 0  above
			//  so it's not a true pure black degenerate (nor all transparent)

			// we still might have not quite true blacks that were classified as "black"
			//	eg. (4,4,4) would fall in the "blackness threshold"
			// we can do better by trying to code those
			// so don't just bail here
			
			// if we don't explicitly detect all-transparent
			//  the drop-through code might use the RGB values to code something funny
			//	(if input was not canonical, but it IS canonical, so that's not true)
			// -> because of canonicalization hiC.dw will be == 0 in either case
										
			// all were in "blackness threshold"
			//  but not true black
			// use the full color bbox						
			loC_colors = loC;
			hiC_colors = hiC;
		}
			
		// use the loC/hiC only of the non-transparent colors
		//	see rrDXT1_IsTransparentOrBlack

		rrColor32BGRA midC_colors = Average(loC_colors,hiC_colors);
		endptr = AddEndPoints_SingleColor_Compact_3C(endptr,midC_colors);

		if ( loC_colors.dw == hiC_colors.dw )
		{
			// degenerate, only one color (that's not transparent or black)
			//	 (this is a little fuzzy because of blackness threshold)
			//	 (there could be other shades of black that we just didn't count)
			// -> no don't return
			// helps to fall through here!
			//return false;
		}
		else
		{
			endptr = AddEndPoints_Force3C(endptr,loC_colors,hiC_colors);
		}
		
		/*
		rrVec3i diagonal_colors = ColorToVec3i(hiC_colors) - ColorToVec3i(loC_colors);
		S32 lensqr = LengthSqr(diagonal_colors);
		if ( lensqr <= 12 )
		{
			return false;
		}
		*/
		
		// @@ fill pData with full color info or reduced ?
		
		/*
		// this seems like a good idea but seems to hurt a bit in practice
		// bc1 -a1 -l3 --w1 r:\rdotestset2\p7_zm_zod_crab_cage_net_c_BC7_UNORM_sRGB_A.tga
		//per-pixel rmse : 5.1956
		//per-pixel rmse : 5.1977
		if ( mode == rrDXT1PaletteMode_Alpha )
		{
			// because of canonicalization
			// transparent pels are black
			// loC will always have dw == 0
			RR_ASSERT( loC.dw == 0 );
			loC = loC_colors;
			avg = ColorToVec3i(midC_colors);
		}
		/**/
		
	}
	
	if ( loC.dw == hiC.dw )
	{
		// degenerate, only one color
		//	already did SingleColor, get out
		return false;
	}	

	rrVec3i diagonal = ColorToVec3i(hiC) - ColorToVec3i(loC);

	if ( LengthSqr(diagonal) <= 12 )
	{
		// very tiny color bbox
		//endPtr = AddEndPoints_TwoColorBest(endPtr,loC,hiC);
		endptr = AddEndPoints_Default(endptr,mode,loC,hiC);
		return false;
	}
	
	// fill rrCompressDXT1_Startup_Data	
	pData->avg = avg;
	pData->diagonal = diagonal;
	pData->sum = sum;
	pData->loC = loC;
	pData->hiC = hiC;	
	
	return true;
}

// rrCompressDXT1_Startup returns false for degenerate blocks that should not continue
//	fills pData if true is returned
bool rrCompressDXT1_Startup(rrCompressDXT1_Startup_Data * pData, rrDXT1Block * pBlock,U32 * pError, const rrColorBlock4x4 & colors, rrDXT1PaletteMode mode, rrDXTCOptions options)
{
	SIMPLEPROFILE_SCOPE(BC1_Startup);

	rrDXT1UnpackedEndPoints endpoints[16];
	rrDXT1UnpackedEndPoints * endptr = endpoints;

	CpuDispatchFlags dispatch = CpuDispatchFlags::init(&options);
	bool result = rrCompressDXT1_Startup_Impl(dispatch,pData,colors,mode,endptr);

	// Score the candidates we came up with
	SINTa count = endptr - endpoints;
	RR_ASSERT( count <= (SINTa)RR_ARRAY_SIZE(endpoints) );
	RR_ASSERT( count >= 1 );

	RAD_ALIGN(rrColor32BGRA, palettes[4 * RR_ARRAY_SIZE(endpoints)], 16);
	TryBatchedEndpoints(dispatch,pBlock,pError,colors,mode,endpoints,(int)count,palettes);

	return result;
}

enum
{
	DXT1_4MEANS_PCA					= 1,	// enable PCA-based seeding (slower)
	DXT1_4MEANS_REDUCED_CANDIDATES	= 2,	// enable reduced candidate set (faster)
};

static void Scalar_Calc_4Means(rrColor32BGRA * means, const rrCompressDXT1_Startup_Data & data, const rrColorBlock4x4 & colors, U32 flags)
{
	rrVec3f avgF = Vec3i_to_Vec3f(data.sum) * (1.f / 16.f);
	rrVec3f pca;

	if ( flags & DXT1_4MEANS_PCA )
	{
		// Cov is stored by diagonals, matching SIMD version
		F32 cov[6] = {};

		// Compute the covariance and also pick the longest diagonal between pixels in the block
		// and the average color; this is guaranteed to at least be something that makes sense for
		// the block (we handled single-color blocks during init) and is symmetric. In particular
		// it's not in the nullspace of the covariance matrix, because we already ruled out
		// the degenerate case (all pixels same) during startup; the longest len we find here
		// is thus nonzero, and so is the contribution from the corresponding pixel to the
		// covariance matrix, so we're good here.
		pca = rrVec3f(1.f,1.f,1.f);
		F32 longest_len2 = 0.0f;

		for LOOP(i,16)
		{
			rrVec3f d = ColorToVec3f( colors.colors[i] ) - avgF;
			float d0 = colors.colors[i].u.b - avgF.x;
			float d1 = colors.colors[i].u.g - avgF.y;
			float d2 = colors.colors[i].u.r - avgF.z;
			cov[0] += d0 * d0; // bb
			cov[1] += d1 * d1; // gg
			cov[2] += d2 * d2; // rr
			cov[3] += d0 * d1; // bg
			cov[4] += d1 * d2; // gr
			cov[5] += d2 * d0; // br

			// Strange-looking summation order to match vector sum_across
			F32 len2 = (d0 * d0 + d2 * d2) + d1 * d1;
			if (len2 > longest_len2)
			{
				pca = d;
				longest_len2 = len2;
			}
		}

		// The only way for this to happen is for all colors to be ==avgF exactly,
		// which is a degenerate case we should have caught in Startup().
		RR_ASSERT( longest_len2 > 0.0f );

		// The covariance matrix is the sum of outer products
		//
		//   C := sum_i (d_i) (d_i)^T
		//
		// It's symmetric positive semidefinite purely from this definition because
		// for any x
		//
		//   dot(x,C x)
		//   = x^T (sum_i (d_i) (d_i)^T)) x
		//   = sum_i (x^T) (d_i) (d_i)^T x
		//   = sum_i dot(x,d_i)^2
		//   >= 0
		//
		// because it's a sum of squares; furthermore our seed "pca" vector is
		// one of the d_i, the longest one. Suppose w.l.o.g. pca=d_1. Then the
		// sum above is >= dot(d_1,d_1) > 0 (see above for argument why pca is
		// not 0). In particular, C * pca != 0.
		//
		// C, being symmetric positive semidefinite, has a full set of real,
		// non-negative eigenvalues lambda_i with pairwise orthogonal eigenvectors
		// v_i. For any vector x written in this basis of eigenvectors
		//
		//   x = sum_i a_i v_i
		//
		// C multiplies the contributions by the corresponding lambda_i (that's
		// just what being diagonalizable means):
		//
		//   C x = sum_i (lambda_i * a_i) v_i
		//
		// For x = pca, since C * pca != 0, that means at least one of the
		// (lambda_i * a_i) is nonzero; since C^2, C^3, ..., C^k (k>=1) written
		// in the same basis are
		//
		//   C^k x = sum_i (lambda_i^k a_i) v_i
		//
		// and we already know that there is one i such that (lambda_i * a_i)
		// is nonzero, lambda_i^k a_i is as well for any k>=1. Therefore,
		// _none_ of the iterates are zero (in exact arithmetic).
		//
		// In practical terms, since C is constructed above from integer values
		// (or rather integer values - avg_F, but avg_F is itself an integer
		// times 1/16, still exact) it can't have tiny near-zero eigenvalues,
		// and our iterates grow very quickly. If it doesn't go to zero in the
		// first iteration, it won't.
		for(int iter=0;iter<PCA_POWER_ITERS/2;iter++)
		{
#define COV_PCA_MULT(cov,pca)										\
			{														\
				F32 b = cov[0]*pca.x + cov[3]*pca.y + cov[5]*pca.z;	\
				F32 g = cov[1]*pca.y + cov[4]*pca.z + cov[3]*pca.x;	\
				F32 r = cov[2]*pca.z + cov[5]*pca.x + cov[4]*pca.y;	\
				pca.x = b;											\
				pca.y = g;											\
				pca.z = r;											\
			}
			COV_PCA_MULT(cov,pca)
			COV_PCA_MULT(cov,pca)
			pca = rrNormalize(pca);
		}
#undef COV_PCA_MULT
	}
	else
	{
		// just diagonal	
		pca = Vec3i_to_Vec3f(data.diagonal);
		pca = rrNormalize(pca);
	}

	// dot the colors in the PCA linear fit direction & seed 4-means
	F32 minDot = FLT_MAX;
	F32 maxDot = -FLT_MAX;
	for LOOP(i,16)
	{
		// Compute dot product with summation matching the SIMD sum_across()
		float dot0 = (colors.colors[i].u.b - avgF.x) * pca.x;
		float dot1 = (colors.colors[i].u.g - avgF.y) * pca.y;
		float dot2 = (colors.colors[i].u.r - avgF.z) * pca.z;
		float dot = (dot0 + dot2) + dot1;
		minDot = RR_MIN(minDot,dot);
		maxDot = RR_MAX(maxDot,dot);
	}

	// make 4 points staggered along the pca line :
	rrVec3f meansf[4];
	meansf[0] = avgF + (0.75f * minDot) * pca;
	meansf[3] = avgF + (0.75f * maxDot) * pca;
	rrVec3f scaled_delta = (meansf[3] - meansf[0]) * (1.f/3.f);
	meansf[1] = meansf[0] + scaled_delta;
	meansf[2] = meansf[3] - scaled_delta;
		
	for LOOP(i,4)
	{
		means[i].u.b = rrRoundAndClampU8(meansf[i].x);
		means[i].u.g = rrRoundAndClampU8(meansf[i].y);
		means[i].u.r = rrRoundAndClampU8(meansf[i].z);
		means[i].u.a = 0xFF;
	}
}

#ifdef DO_BUILD_SSE4
static void SSE4_Calc_4Means(rrColor32BGRA * means, const rrCompressDXT1_Startup_Data & data, const rrColorBlock4x4 & colors, U32 flags)
{
	Vec128_S32 force_a_opaque(0,0,0,255);
	Vec128_S32 sum(data.sum.x, data.sum.y, data.sum.z, 16*255); // Force average to have 255 in alpha
	VecF32x4 avgF = sum.to_f32() * VecF32x4(1.f / 16.f);
	VecF32x4 pca = VecF32x4::zero();

	if ( flags & DXT1_4MEANS_PCA )
	{
		VecF32x4 cov0 = VecF32x4::zero();
		VecF32x4 cov1 = VecF32x4::zero();

		// Compute the covariance and also pick the longest diagonal between pixels in the block
		// and the average color; this is guaranteed to at least be something that makes sense for
		// the block (we handled single-color blocks during init) and is symmetric. In particular
		// it's not in the nullspace of the covariance matrix, because we already ruled out
		// the degenerate case (all pixels same) during startup; the longest len we find here
		// is thus nonzero, and so is the contribution from the corresponding pixel to the
		// covariance matrix, so we're good here.
		VecF32x4 longest_len2 = VecF32x4::zero();

		for LOOP(i,16)
		{
			Vec128_S32 pixint = Vec128_U8::loadu_lo32(&colors.colors[i]).to_s32_lo() | force_a_opaque;
			VecF32x4 d = pixint.to_f32() - avgF;
			VecF32x4 d2 = d * d;
			cov0 += d2;				// bb gg rr 0
			cov1 += d * d.yzxw();	// bg gr br 0

			VecF32x4 len2 = d2.sum_across();
			if (_mm_ucomigt_ss(len2, longest_len2)) // if (len2 > longest_len2)
			{
				pca = d;
				longest_len2 = len2;
			}
		}

		// The only way for this to happen is for all colors to be ==avgF exactly,
		// which is a degenerate case we should have caught in Startup().
		RR_ASSERT( longest_len2.scalar_x() > 0.0f );

		VecF32x4 cov2 = cov1.zxyw();

		// The covariance matrix is the sum of outer products
		//
		//   C := sum_i (d_i) (d_i)^T
		//
		// It's symmetric positive semidefinite purely from this definition because
		// for any x
		//
		//   dot(x,C x)
		//   = x^T (sum_i (d_i) (d_i)^T)) x
		//   = sum_i (x^T) (d_i) (d_i)^T x
		//   = sum_i dot(x,d_i)^2
		//   >= 0
		//
		// because it's a sum of squares; furthermore our seed "pca" vector is
		// one of the d_i, the longest one. Suppose w.l.o.g. pca=d_1. Then the
		// sum above is >= dot(d_1,d_1) > 0 (see above for argument why pca is
		// not 0). In particular, C * pca != 0.
		//
		// C, being symmetric positive semidefinite, has a full set of real,
		// non-negative eigenvalues lambda_i with pairwise orthogonal eigenvectors
		// v_i. For any vector x written in this basis of eigenvectors
		//
		//   x = sum_i a_i v_i
		//
		// C multiplies the contributions by the corresponding lambda_i (that's
		// just what being diagonalizable means):
		//
		//   C x = sum_i (lambda_i * a_i) v_i
		//
		// For x = pca, since C * pca != 0, that means at least one of the
		// (lambda_i * a_i) is nonzero; since C^2, C^3, ..., C^k (k>=1) written
		// in the same basis are
		//
		//   C^k x = sum_i (lambda_i^k a_i) v_i
		//
		// and we already know that there is one i such that (lambda_i * a_i)
		// is nonzero, lambda_i^k a_i is as well for any k>=1. Therefore,
		// _none_ of the iterates are zero (in exact arithmetic).
		//
		// In practical terms, since C is constructed above from integer values
		// (or rather integer values - avg_F, but avg_F is itself an integer
		// times 1/16, still exact) it can't have tiny near-zero eigenvalues,
		// and our iterates grow very quickly. If it doesn't go to zero in the
		// first iteration, it won't.
		for(int iter=0;iter<PCA_POWER_ITERS/2;iter++)
		{
			pca = pca * cov0 + pca.yzxw() * cov1 + pca.zxyw() * cov2;
			pca = pca * cov0 + pca.yzxw() * cov1 + pca.zxyw() * cov2;
			// Normalize result
			VecF32x4 len_sq = (pca * pca).sum_across();
			VecF32x4 len_positive = len_sq.cmp_gt(VecF32x4::zero());
			pca = (pca / len_sq.sqrt()) & len_positive;
		}
	}
	else
	{
		// just diagonal
		Vec128_S32 diag_s32 = Vec128_S32(data.diagonal.x,data.diagonal.y,data.diagonal.z,0); // Zero out alpha channel
		VecF32x4 diag = diag_s32.to_f32();

		// Normalize result
		VecF32x4 len_sq = (diag * diag).sum_across();
		VecF32x4 len_positive = len_sq.cmp_gt(VecF32x4::zero());
		pca = (diag / len_sq.sqrt()) & len_positive;
	}

	// dot the colors in the PCA linear fit direction & seed 4-means
	VecF32x4 vMinMaxDot = VecF32x4(FLT_MAX);
	for LOOP(i,16)
	{
		Vec128_S32 pixint = Vec128_U8::loadu_lo32(&colors.colors[i]).to_s32_lo() | force_a_opaque;
		VecF32x4 dot = ((pixint.to_f32() - avgF) * pca).sum_across();
		vMinMaxDot = vmin(vMinMaxDot, dot ^ VecF32x4(0.f, -0.f, 0.f, 0.f));
	}
	VecF32x4 scaledMinMaxDot = vMinMaxDot * VecF32x4(0.75f, -0.75f, 0.f, 0.f); // (minDot*.75, maxDot*.75, 0, 0)

	// make 4 points staggered along the pca line :
	VecF32x4 meansf0 = avgF + pca * scaledMinMaxDot.xxxx();
	VecF32x4 meansf3 = avgF + pca * scaledMinMaxDot.yyyy();
	VecF32x4 scaled_delta = (meansf3 - meansf0) * VecF32x4(1.f/3.f);
	VecF32x4 meansf1 = meansf0 + scaled_delta;
	VecF32x4 meansf2 = meansf3 - scaled_delta;

	Vec128_S16 means01_s16 = meansf0.to_int32_round().to_s16_sat(meansf1.to_int32_round());
	Vec128_S16 means23_s16 = meansf2.to_int32_round().to_s16_sat(meansf3.to_int32_round());
	Vec128_U8  means0123_u8 = means01_s16.to_u8_sat(means23_s16);
	means0123_u8.storeu(means);
}
#endif

bool rrCompressDXT1_4Means(const rrCompressDXT1_Startup_Data & data,rrDXT1Block * pBlock,U32 * pError, const rrColorBlock4x4 & colors, rrDXT1PaletteMode mode, U32 flags)
{
	SIMPLEPROFILE_SCOPE(BC1_4Means);

	// NOTE: the use of 4-Means here has nothing in particular to do with the 4
	// "palette entries" in a BC1 block; we still make endpoint pairs from just
	// pairs of colors (we have to, it's not like BC1 lets us freely pick
	// in-between colors), but the 4-Means does a decent job at giving us
	// interesting options off the PCA line.

	// means are indexed like 0,1,2,3 in order, not the DXT1 order of 0,2,3,1
	rrColor32BGRA means[4];

	// make initial 4 means :
#ifdef DO_BUILD_SSE4
	SSE4_Calc_4Means(means, data, colors, flags);
#else
	Scalar_Calc_4Means(means, data, colors, flags);
#endif

	{
	//SIMPLEPROFILE_SCOPE(BC1_4Means_FindMeans);

	DXT1_SolveRGB4Means(means, colors);
	
	// We may have ended up taking means straight from colors.colors, including
	// the alpha value; the 4-means loop ignores alpha, but make sure to force
	// alpha to 255 for the following
	for(int i=0;i<4;i++)
		means[i].u.a = 255;

	}
	
	// add all endpoint pairs we want to try
	// Compress_TwoColorBest_AddEndPoints can add 8  (was 10)
	//	8*7 = 56
	
	static const int NUM_PAIRS = 10*7;
	rrDXT1UnpackedEndPoints endpoints[NUM_PAIRS];
	rrDXT1UnpackedEndPoints * endptr = endpoints;

	if ( flags & DXT1_4MEANS_REDUCED_CANDIDATES )
	{
		endptr = AddEndPoints_BothWays(endptr,means[0],means[1]);
		endptr = AddEndPoints_BothWays(endptr,means[0],means[2]);
		endptr = AddEndPoints_BothWays(endptr,means[0],means[3]);

		endptr = AddEndPoints_BothWays(endptr,means[1],means[2]);
		endptr = AddEndPoints_BothWays(endptr,means[1],means[3]);

		endptr = AddEndPoints_BothWays(endptr,means[2],means[3]);
	}
	else
	{
		endptr = AddEndPoints_TwoColorBest(endptr,means[0],means[3]);
		endptr = AddEndPoints_TwoColorBest(endptr,means[1],means[2]);
		endptr = AddEndPoints_TwoColorBest(endptr,means[0],means[2]);
		endptr = AddEndPoints_TwoColorBest(endptr,means[1],means[3]);

		endptr = AddEndPoints_TwoColorBest(endptr,means[0],means[1]);
		endptr = AddEndPoints_TwoColorBest(endptr,means[2],means[3]);
		endptr = AddEndPoints_TwoColorBest(endptr, Average(means[0],means[1]), Average(means[2],means[3]) );
	}
	
	RR_ASSERT( (SINTa)(endptr - endpoints) <= (SINTa)RR_ARRAY_SIZE(endpoints) );
	
	// If we have 1-bit alpha, endpoints that select 4-color mode do us no good, so we can
	// remove them all. Filter them out before the trial loop.
	//
	// Likewise, in reduced candidates mode, we only try 4-color unless we need 3-color
	// for alpha.
	U32 target_modes;
	if ( data.has_any_alpha )
		target_modes = FilterEndpoints_Allow3c;
	else
		target_modes = ( flags & DXT1_4MEANS_REDUCED_CANDIDATES ) ? FilterEndpoints_Allow4c : FilterEndpoints_AllowBoth;

	endptr = FilterEndpoints(endpoints,endptr,target_modes);
	int count = (int)(endptr - endpoints);

	// Try all the candidates
	RAD_ALIGN(rrColor32BGRA, palettes[4 * NUM_PAIRS], 16);
	TryBatchedEndpoints(data.dispatch,pBlock,pError,colors,mode,endpoints,count,palettes);

	return true;
}

//===================================================================

// 0 = VeryFast
void rrCompressDXT1_0(rrDXT1Block * pBlock,U32 * pError, const rrColorBlock4x4 & colors, rrDXTCOptions options, rrDXT1PaletteMode mode)
{
	// @@ : note : this is "VeryFast"
	//	this is really a place-holder
	//	should replace with a good fast version
	//	using SSE2 and the simple divide method or whatever
	//	also doing block-at-a-time is obviously not ideal for VeryFast
	
	*pError = RR_DXTC_INIT_ERROR;

	rrCompressDXT1_Startup_Data data;
	if ( ! rrCompressDXT1_Startup(&data,pBlock,pError,colors,mode,options) )
		return;

	if ( ! rrCompressDXT1_4Means(data,pBlock,pError,colors,mode,DXT1_4MEANS_REDUCED_CANDIDATES) )
		return;
	
	// added 06-01-2019 :
	// @@ this should be skipped on flat blocks
	//	 and probably other cases where it's unlikely to help
	DXT1_OptimizeEndPointsFromIndices_Inherit_Reindex(data.dispatch,pBlock,pError,colors,mode);
}

// 1 = Fast
void rrCompressDXT1_1(rrDXT1Block * pBlock,U32 * pError, const rrColorBlock4x4 & colors, rrDXTCOptions options, rrDXT1PaletteMode mode)
{
	*pError = RR_DXTC_INIT_ERROR;
	
	rrCompressDXT1_Startup_Data data;
	if ( ! rrCompressDXT1_Startup(&data,pBlock,pError,colors,mode,options) )
		return;

	if ( ! rrCompressDXT1_4Means(data,pBlock,pError,colors,mode,0) )
		return;
		
	DXT1_OptimizeEndPointsFromIndicesIterative(data.dispatch,pBlock,pError,colors,mode);
	
}

// 2 = Slow
void rrCompressDXT1_2(rrDXT1Block * pBlock,U32 * pError, const rrColorBlock4x4 & colors, rrDXTCOptions options, rrDXT1PaletteMode mode)
{
	*pError = RR_DXTC_INIT_ERROR;
	
	rrCompressDXT1_Startup_Data data;
	if ( ! rrCompressDXT1_Startup(&data,pBlock,pError,colors,mode,options) )
		return;

	if ( ! rrCompressDXT1_4Means(data,pBlock,pError,colors,mode,0) )
		return;
		
	// 8 means here is not worth it, a lot slower and no big gains :
	//rrCompressDXT1_8Means(pBlock,pError,colors,mode);

	DXT1_OptimizeEndPointsFromIndicesIterative(data.dispatch,pBlock,pError,colors,mode);
	
	//DXT1_AnnealBlock(pBlock,pError,colors,mode);

	DXT1_GreedyOptimizeBlock(data,pBlock,pError,colors,mode,false);
	
	/**/
	
	// verify *pError :
	RR_ASSERT( *pError == DXT1_ComputeSSD_OneBitTransparent(colors,*pBlock,mode) );
}

// 3 = VerySlow + Reference
void rrCompressDXT1_3(rrDXT1Block * pBlock,U32 * pError, const rrColorBlock4x4 & colors, rrDXTCOptions options, rrDXT1PaletteMode mode, rrDXTCLevel level)
{
	SIMPLEPROFILE_SCOPE(BC1_Level3);

	*pError = RR_DXTC_INIT_ERROR;
	
	rrCompressDXT1_Startup_Data data;
	if ( ! rrCompressDXT1_Startup(&data,pBlock,pError,colors,mode,options) )
	{
		// linear_ramp1.BMP still wants Optimize
		//	nobody else cares
		DXT1_GreedyOptimizeBlock(data,pBlock,pError,colors,mode,true);
		return;
	}

	// two approaches here, seem to come out quite similar
	//	in terms of overall rmse and run time
	// rmse is similar on all files
	// run time is NOT , some times one approach is much faster, but it's not monotonic
	// overall 8means+squish seems to be slightly better rmse & run time (than 4means + all pairs)

	#if 0
	
	// 4means + all pairs
	
	bool non_degenerate = rrCompressDXT1_4Means(data,pBlock,pError,colors,mode,0);

	if ( ! non_degenerate )
	{
		// linear_ramp1.BMP still wants Optimize
		//	nobody else cares
		DXT1_GreedyOptimizeBlock(pBlock,pError,colors,mode);
		return;
	}
	
	DXT1_OptimizeEndPointsFromIndicesIterative(pBlock,pError,colors,mode);
	
	if ( level >= rrDXTCLevel_Reference ) 
	{
		// Compress_TryAllPairs_Heavy does its own DXT1_OptimizeEndPointsFromIndicesIterative
		//  this is pretty slow and rarely helps much
		//	 it helps most on the rare weirdo images (frymire/serrano)	
		Compress_TryAllPairs_Heavy(pBlock,pError,colors,mode);
	}
	
	//rmse_total = 382.464

	#else

	// alternate approach : 4Means + PCA
	bool non_degenerate = rrCompressDXT1_4Means(data,pBlock,pError,colors,mode,DXT1_4MEANS_PCA);
	
	if ( ! non_degenerate )
	{
		// linear_ramp1.BMP still wants Optimize for degenerate blocks
		//	nobody else cares
		DXT1_GreedyOptimizeBlock(data,pBlock,pError,colors,mode,true);
		return;
	}

	DXT1_OptimizeEndPointsFromIndicesIterative(data.dispatch,pBlock,pError,colors,mode);
	
	if ( level >= rrDXTCLevel_Reference )
	{
		// bc1difficult :
		// with neither :
		// rmse_total = 119.040
		// rmse_total = 118.205 without rrCompressDXT1_PCA_Squish_All_Clusters (yes Compress_TryAllPairs_Heavy)
		// rmse_total = 118.398 without Compress_TryAllPairs_Heavy (yes rrCompressDXT1_PCA_Squish_All_Clusters)
		// rmse_total = 118.166 with Compress_TryAllPairs_Heavy (and rrCompressDXT1_PCA_Squish_All_Clusters)
	
		// can try squish here too to see if it finds something different
		// @@ helps only a little and very slow -> I think this should go
		//	 leaving for now as "ground truth"
		// NOTE(fg): Squish-like removed 2021-09-22.
		// We have too many old experiments in here.
		//rrCompressDXT1_PCA_Squish_All_Clusters(data,pBlock,pError,colors,mode);
		
		// Compress_TryAllPairs_Heavy does its own DXT1_OptimizeEndPointsFromIndicesIterative
		//  this is pretty slow and rarely helps much
		//	 it helps most on the rare weirdo images (frymire/serrano)	
		Compress_TryAllPairs_Heavy(data.dispatch,pBlock,pError,colors,mode);
	}
	
	#endif

	if ( *pError == 0 ) return; // pretty rare but may as well
	
	if ( 1 ) // level >= rrDXTCLevel_Reference )
	{
		// @@ alternative to Anneal that should be considered
		//	 is just to do a greedy optimize but with randomized larger steps
		//	(you would have to consider joint endpoint moves like dilations & contractions)

		/*
		// Anneal in VerySlow ?
		// yes I guess so
		
		rmse_total = 307.686 Slow
		rmse_total = 307.024 VerySlow
		rmse_total = 306.321 VerySlow with Anneal
		rmse_total = 305.705 Reference
		*/

		DXT1_AnnealBlock(data,pBlock,pError,colors,mode,level >= rrDXTCLevel_Reference);
	}
	
	DXT1_GreedyOptimizeBlock(data,pBlock,pError,colors,mode,true);
}

//================================================


#define NUM_WIGGLES	(6)	// number of non-null wiggles

// rrColor32BGRA is ARGB in shifts
static const S32 c_wiggledw_delta[8] = { 1<<16,-(1<<16), 1<<8,-(1<<8), 1,-1, 0,0 };

static RADFORCEINLINE rrColorUnpacked565 Wiggle(const rrColorUnpacked565 & color,int how)
{
	U32 dw;
	rrColorUnpacked565 ret;
	
	dw = color.dw;
	RR_ASSERT( (dw & 0xFF1F3F1F) == dw );
	dw += (U32)c_wiggledw_delta[how];
	// if we went out of allowed range on this color,
	// some bits outside of 0x1F3F1F are on; instead
	// of clamping, we can just return the original
	// value (which works out to the same thing)
	ret.dw = (dw & (~0xFF1F3F1F)) ? color.dw : dw;
	
	return ret;
}

// This updates the endpoint and error, but not the indices, that's done outside
static void DXT1_GreedyOptimizeBlock_Inner(const rrCompressDXT1_Startup_Data & data,rrDXT1Block * pBlock,U32 * pError, const rrColorBlock4x4 & colors, rrDXT1PaletteMode mode, bool do_joint_optimization)
{
	SIMPLEPROFILE_SCOPE(BC1_GreedyOpt);
	
	// Greedy optimization - do after Annealing

	RR_ASSERT( *pError == DXT1_ComputeSSD_OneBitTransparent(colors,*pBlock,mode) );

	// these are unpacked to bytes but NOT unquantized :	
	rrColorUnpacked565 best0(pBlock->c0);
	rrColorUnpacked565 best1(pBlock->c1);
	DXT1_FindErrorsContext ctx;

	ctx.init(data.dispatch, colors);
	
	for(;;)
	{
		rrColorUnpacked565 start0 = best0;
		rrColorUnpacked565 start1 = best1;

		const int MAX_TRIALS = 7*7;
		rrDXT1UnpackedEndPoints endpoints[MAX_TRIALS];
		int count = 0;
		
		// do_joint_optimization : 
		//	N*N pair wiggles, like end0 +1 in B and end1 -1 in R 
		//	or N+N independent endpoint wiggles (like BC7 does)
		
		// it's a pretty big speed difference
		//	but there is some decent quality available from do_joint_optimization
		// -> for now I'm turning off joint_optimization at level 2 (Slow)
		//	 leaving it on at level >= 3 (VerySlow)
		//	level 3 is annealing too so the speed impact of changing this isn't enormous
		
		if ( do_joint_optimization )
		{
			
			// try all wiggles :
			// 7*7 == 49 trials (actually 48, the both null is skipped)
			for(int w1=0;w1<=NUM_WIGGLES;w1++)
			{
				rrColorUnpacked565 c0 = Wiggle(start0,w1);
				
				for(int w2=0;w2<=NUM_WIGGLES;w2++)
				{
					rrColorUnpacked565 c1 = Wiggle(start1,w2);
					
					if ( c0 == start0 && c1 == start1 )
						continue;
									
					// if you have alpha, reject 4c mode :
					if ( data.has_any_alpha && c0 > c1 )
						continue;

					endpoints[count].c[0] = c0;
					endpoints[count].c[1] = c1;
					++count;
				}
			}

		}
		else
		{
			// N+N instead of N*N
			for(int w1=0;w1<NUM_WIGGLES;w1++)
			{
				rrColorUnpacked565 c0 = Wiggle(start0,w1);
				if ( c0 == start0 )
					continue;
						
				// if you have alpha, reject 4c mode :
				if ( data.has_any_alpha && c0 > start1 )
					continue;

				endpoints[count].c[0] = c0;
				endpoints[count].c[1] = start1;
				++count;
			}
			
			for(int w2=0;w2<NUM_WIGGLES;w2++)
			{
				rrColorUnpacked565 c1 = Wiggle(start1,w2);
				
				if ( c1 == start1 )
					continue;
								
				// if you have alpha, reject 4c mode :
				if ( data.has_any_alpha && start0 > c1 )
					continue;

				endpoints[count].c[0] = start0;
				endpoints[count].c[1] = c1;
				++count;
			}
		}

		// Score all of the current options
		RAD_ALIGN(rrColor32BGRA, palettes[4 * MAX_TRIALS], 16);

		DXT1_ComputePalette_Batched(endpoints,count,palettes,mode);

		U32 best_err_and_i = ctx.find_best_palette(palettes, count);
		U32 best_err = best_err_and_i >> DXT1_FindErrorsContext::COUNT_SHIFT;
		if ( best_err < *pError )
		{
			U32 best_i = best_err_and_i & (DXT1_FindErrorsContext::COUNT_LIMIT - 1);

			*pError = best_err;
			best0 = endpoints[best_i].c[0];
			best1 = endpoints[best_i].c[1];
			pBlock->c0 = best0.pack();
			pBlock->c1 = best1.pack();
			if ( best_err == 0 )
				return;
		}
		else // no improvement found, we're done
			break;
	}
}

void DXT1_GreedyOptimizeBlock(const rrCompressDXT1_Startup_Data & data,rrDXT1Block * pBlock,U32 * pError, const rrColorBlock4x4 & colors, rrDXT1PaletteMode mode, bool do_joint_optimization)
{
	// If the error is already 0, nothing to do
	if ( *pError == 0 )
		return;

	U32 origError = *pError;

	DXT1_GreedyOptimizeBlock_Inner(data,pBlock,pError,colors,mode,do_joint_optimization);

	// If we found an improvement, we need to figure out what those indices are!
	if ( *pError != origError )
	{
		RAD_ALIGN(rrColor32BGRA, palette[4], 16);
		DXT1_ComputePalette(pBlock->c0,pBlock->c1,palette,mode);

		U32 solved_err;
		pBlock->indices = DXT1_FindIndices(data.dispatch,colors,palette,&solved_err);
		RR_ASSERT( solved_err == *pError );
	}
}

#if defined(DO_BUILD_SSE4)
template<int t_shift>
static RADFORCEINLINE Vec128_S32 Wiggle4x(Vec128_S32 colors, Vec128_U8 control)
{
	// Control bytes are replicated 4x
	// we use three bits control[t_shift + 2:t_shift]
	//
	// if low control bit (which selects sign) is set, make corresponding byte -1, else 0
	Vec128_U8 c_sign_bit { 1 << t_shift };
	Vec128_U8 v_control_sign_mask = (control & c_sign_bit).cmp_eq(c_sign_bit);

	// Select channel to wiggle
	// control=0,1 is +-R (channel 2 in BGRA byte order)
	// control=2,3 is +-G (channel 1 in BGRA byte order)
	// control=4,5 is +-B (channel 0 in BGRA byte order)
	// control=6,7 is nop
	//
	// -> light up:
	//    B when (control & 6) == 4,
	//    G when (control & 6) == 2,
	//    R when (control & 6) == 0,
	//    A never.
	//
	// so we check (control & 6) == { 4,2,0,-1 }.
	// That last value can be anything that never equals (control & 6).
	//
	// This materializes -1 when channel matches, not +1, but that's fine
	// we just keep the signs flipped and subtract instead of adding
	Vec128_U8 c_channel_compare = Vec128_U8::repeat4(4 << t_shift, 2 << t_shift, 0 << t_shift, 255);
	Vec128_U8 v_wiggle_amount_neg0 = (control & Vec128_U8(6 << t_shift)).cmp_eq(c_channel_compare);

	// Negate value where required
	Vec128_U8 v_wiggle_amount_neg = (v_wiggle_amount_neg0 ^ v_control_sign_mask) - v_control_sign_mask;

	// Perform actual wiggle
	Vec128_U8 v_wiggled = colors.u8() - v_wiggle_amount_neg;
	v_wiggled = vmax(v_wiggled.s8(), Vec128_S8::zero()).u8();
	v_wiggled = vmin(v_wiggled, Vec128_U8::repeat4(31, 63, 31, 0));

	return v_wiggled.s32();
}
#endif

void DXT1_AnnealBlock(const rrCompressDXT1_Startup_Data & data,rrDXT1Block * pBlock,U32 * pError, const rrColorBlock4x4 & colors, 
	rrDXT1PaletteMode mode, bool expensive_mode)
{
	SIMPLEPROFILE_SCOPE(BC1_Anneal);

	rrColorUnpacked565 cur0(pBlock->c0);
	rrColorUnpacked565 cur1(pBlock->c1);
	U32 curError = *pError;
	U32 initialError = curError;
	
	RR_ASSERT( *pError == DXT1_ComputeSSD_OneBitTransparent(colors,*pBlock,mode) );

	rrDXT1UnpackedEndPoints best = { { cur0, cur1 } };
	U64 rand_state = rrRand64Simple_SeedFromU64Array((const U64 *)colors.colors,8);

	const int NITER = 256;
	RAD_ALIGN(U8, rng_bytes[NITER], 8);
	RR_COMPILER_ASSERT(NITER % 8 == 0);

	// Generate all the random bytes up front
	for(int i=0;i<NITER;i+=8)
	{
		U64 r = rrRand64Simple(&rand_state);
		RR_PUT64_LE(rng_bytes + i,r);
	}

	RAD_ALIGN(rrColor32BGRA, palette[4*4], 16);
	DXT1_FindErrorsContext ctx;
	ctx.init(data.dispatch, colors);

	// Threshold linearly decreases over time:
	// threshold(i) = t(i) = base - i*step
	const int thresholdBase = 253;
	const int thresholdStep = 4;

	int initial_time = 0;

#if 0 // DISABLED for 2.9.7: this is promising but needs more tuning
	if ( !expensive_mode )
	{
		// Idea: skip initial steps that would let us increase the error significantly above its
		// currrent value. Doesn't cost us much in terms of result quality but _way_ faster.
		//
		// t(i) = base - i*step  <=> i*step = base - t(i)
		int adjustedCurError = curError/3 + 30; // tweakable
		initial_time = RR_MAX(thresholdBase - adjustedCurError, 0) / thresholdStep;
	}
#endif

#if defined(DO_BUILD_SSE4)
	Vec128_S32 v_cur0(cur0.dw);
	Vec128_S32 v_cur1(cur1.dw);
	Vec128_S32 v_curErr(curError);
	Vec128_S32 v_curBestErr(*pError);
	Vec128_S32 v_thresh(thresholdBase - initial_time*thresholdStep);

	Vec128_U8 c_broadcast4x { 0,0,0,0, 1,1,1,1, 2,2,2,2, 3,3,3,3 };

	for (int time = initial_time; time < 64; ++time)
	{
		// Get the RNG wiggle control bytes, replicate 4x each
		Vec128_U8 v_rng_bytes = Vec128_U8::loadu_lo32(rng_bytes + time*4).shuf(c_broadcast4x);

		// Wiggle endpoints
		Vec128_S32 v_ep0 = Wiggle4x<0>(v_cur0, v_rng_bytes);
		Vec128_S32 v_ep1 = Wiggle4x<3>(v_cur1, v_rng_bytes);

		// Put into the order that gives us a transparent color if required
		if (data.has_any_alpha)
		{
			Vec128_S32 t = v_ep0;
			v_ep0 = vmin(v_ep0, v_ep1);
			v_ep1 = vmax(t, v_ep1);
		}

		// Do the trials
		RAD_ALIGN(rrDXT1UnpackedEndPoints, ep[4], 16);
		RAD_ALIGN(U32, err[4], 16);

		v_ep0.unpack_lo(v_ep1).storeu(ep + 0);
		v_ep0.unpack_hi(v_ep1).storeu(ep + 2);
		DXT1_ComputePalette_Batched(ep,4,palette,mode);
		ctx.eval_palettes(&ctx,err,palette,4);

		// Check for improvements over global best
		Vec128_S32 v_err = Vec128_S32::loadu(err);
		if (v_curBestErr.cmp_gt(v_err).any())
		{
			// Find best error in batch, breaking ties towards smaller i
			Vec128_S32 v_best_err = v_err.shl<2>() | Vec128_S32(0,1,2,3);
			S32 best_overall = reduce_min(v_best_err);
			SINTa i = best_overall & 3;
			S32 best_err = best_overall >> 2;

			RR_ASSERT(best_err >= 0 && (U32)best_err < *pError);
			*pError = (U32)best_err;
			v_curBestErr = Vec128_S32(best_err);
			best = ep[i];
			if (best_err == 0)
				goto done;
		}

		// Regular acceptance processing (threshold accepting)
		// accept new if v_err - v_curErr < thresh
		Vec128_S32 v_should_accept = v_thresh.cmp_gt(v_err - v_curErr);
		v_curErr = v_should_accept.select(v_err, v_curErr);
		v_cur0 = v_should_accept.select(v_ep0, v_cur0);
		v_cur1 = v_should_accept.select(v_ep1, v_cur1);

		// Decrease threshold over time
		v_thresh -= Vec128_S32(thresholdStep);
	}
#else
	rrDXT1UnpackedEndPoints cur[4];
	U32 curErr[4];
	for(int i=0;i<4;i++)
	{
		cur[i].c[0] = cur0;
		cur[i].c[1] = cur1;
		curErr[i] = curError;
	}
	
	for(int time=initial_time;time<64;time++)
	{
		// Set up 4 parallel trials
		rrDXT1UnpackedEndPoints ep[4];
		U32 err[4];

		for (int i=0;i<4;i++)
		{
			U8 r = rng_bytes[time*4 + i];

			rrColorUnpacked565 c0 = Wiggle(cur[i].c[0],r & 7);
			rrColorUnpacked565 c1 = Wiggle(cur[i].c[1],(r >> 3) & 7);

			if ( data.has_any_alpha && c0 > c1 )
				RR_NAMESPACE::swap(c0,c1);

			ep[i].c[0] = c0;
			ep[i].c[1] = c1;
		}

		DXT1_ComputePalette_Batched(ep,4,palette,mode);
		ctx.eval_palettes(&ctx,err,palette,4);

		const int thresh = thresholdBase - time*thresholdStep;
		for (int i=0;i<4;i++)
		{
			int diff = (int)err[i] - (int)curErr[i];
			if ( diff >= thresh )
				continue;

			if ( err[i] < *pError )
			{
				*pError = err[i];
				best = ep[i];
				if ( err[i] == 0 )
					goto done;
			}

			curErr[i] = err[i];
			cur[i] = ep[i];
		}
	}
#endif

done:
	// If we found an improvement in the annealing loop, we determined
	// the new error but not the indices; rectify this now.
	if ( *pError != initialError )
	{
		U32 solved_err;
		pBlock->c0 = best.c[0].pack();
		pBlock->c1 = best.c[1].pack();
		DXT1_ComputePalette(pBlock->c0,pBlock->c1,palette,mode);
		pBlock->indices = DXT1_FindIndices(data.dispatch,colors,palette,&solved_err);

		RR_ASSERT( solved_err == *pError );
	}
	
	// after Anneal you want to further DXT1_GreedyOptimizeBlock
	//	to do greedy steps if any are available
	
}

//=========================================================================

/*

OptimizeEndPointsFromIndices :

Simon's lsqr to optimize end points from indices

*/

BC1EndpointLLSSolver::BC1EndpointLLSSolver(bool fourc)
	: AA(0), AB(0), BB(0), fourc(fourc),
	normalization_factor(fourc ? 3.0f : 2.0f)
{
	for (int i = 0; i < 4; ++i)
	{
		AX[i] = 0.0f;
		BX[i] = 0.0f;
	}
}

void BC1EndpointLLSSolver::accumulate(const rrColorBlock4x4 & colors, U32 indices)
{
	// can just scale up weights to make them ints
	// also makes the determinant an int so we can detect the degenerate cases exactly
	struct WeightTable
	{
		U32 products[4]; // can accumulate AA, AB, BB in parallel in one word
		F32 ab[4*2 + 2]; // pairs of weights A, B with 2 padding at the end
	};

#define WEIGHTS(a,b) ((a*a) | ((a*b) << 8) | ((b*b) << 16))
	static const WeightTable c_weights[2] =
	{
		// 3-color mode
		{
			{ WEIGHTS(2,0), WEIGHTS(0,2), WEIGHTS(1,1), WEIGHTS(0,0) },
			{ 2,0, 0,2, 1,1, 0,0, 0,0 },
		},
		// 4-color mode
		{
			{ WEIGHTS(3,0), WEIGHTS(0,3), WEIGHTS(2,1), WEIGHTS(1,2) },
			{ 3,0, 0,3, 2,1, 1,2, 0,0 },
		},
	};
#undef WEIGHTS
	const WeightTable * pWeights = &c_weights[fourc ? 1 : 0];
	U32 tindices = indices;
	U32 products = 0;

#if defined(DO_BUILD_SSE4) || defined(DO_BUILD_NEON64)
	VecF32x4 vAX = VecF32x4::loadu(AX);
	VecF32x4 vBX = VecF32x4::loadu(BX);

	for(int i=0;i<16;i++)
	{
		U32 index = tindices & 3;
		tindices >>= 2;

		// Accumulate AA, AB, BB products; these are all in [0,9], so we can keep them
		// all bitpacked in a 32-bit int and unpack later.
		products += pWeights->products[index];

		Vec128_S32 pixint = Vec128_U8::loadu_lo32(&colors.colors[i]).to_s32_lo();
		VecF32x4 X = VecF32x4::from_int32(pixint);
		VecF32x4 Weights = VecF32x4::loadu(pWeights->ab + index*2);

		vAX += Weights.dup<0>() * X;
		vBX += Weights.dup<1>() * X;
	}

	vAX.storeu(AX);
	vBX.storeu(BX);
#else
	rrVec3f vAX(AX[0], AX[1], AX[2]);
	rrVec3f vBX(BX[0], BX[1], BX[2]);

	for(int i=0;i<16;i++)
	{
		U32 index = tindices & 3;
		tindices >>= 2;

		// Accumulate AA, AB, BB products; these are all in [0,9], so we can keep them
		// all bitpacked in a 32-bit int and unpack later.
		products += pWeights->products[index];

		rrVec3f X = ColorToVec3f( colors.colors[i] );
		vAX += pWeights->ab[index*2 + 0] * X;
		vBX += pWeights->ab[index*2 + 1] * X;
	}

	AX[0] = vAX.x;
	AX[1] = vAX.y;
	AX[2] = vAX.z;
	BX[0] = vBX.x;
	BX[1] = vBX.y;
	BX[2] = vBX.z;
#endif

	// Unpack packed weights
	AA += (products >> 0) & 0xff;
	AB += (products >> 8) & 0xff;
	BB += (products >> 16) & 0xff;

}

bool BC1EndpointLLSSolver::solve_endpoints(rrColor565Bits * pA, rrColor565Bits * pB) const
{
	int det = AA*BB - AB*AB;
	if ( det == 0 )
		return false;
	
	// have to multiply invDet by the normalization factor that was used on weights :
	float invDet = normalization_factor / (float) det;
	rrVec3f vAX(AX[0], AX[1], AX[2]);
	rrVec3f vBX(BX[0], BX[1], BX[2]);
	
	rrVec3f vA = float(  BB * invDet) * vAX + float(- AB * invDet ) * vBX;
	rrVec3f vB = float(- AB * invDet) * vAX + float(  AA * invDet ) * vBX;
	
	// @@ just quantizing here may be sub-optimal
	//	when near a boundary, should try both ways and see which gives lower error

	*pA = Vec3fToQuantized565_RN(vA);
	*pB = Vec3fToQuantized565_RN(vB);
	return true;
}

bool DXT1_OptimizeEndPointsFromIndices_Raw(U32 * pEndPoints, const U32 indices, bool fourc, const rrColorBlock4x4 & colors)
{
	BC1EndpointLLSSolver solver(fourc);

	solver.accumulate(colors, indices);

	rrColor565Bits qA, qB;
	if ( ! solver.solve_endpoints(&qA,&qB) )
		return false;

	// Try to swap into the desired order, if possible.
	// In case where both modes are possible, we can always hit 3c
	// (since it triggers on qA <= qB), but not always 4c (it's
	// not available to us when they're equal).

	// qA <= qB is 3c
	// qA >  qB is 4c
	if ( ( qA.w > qB.w ) != fourc )
		RR_NAMESPACE::swap(qA.w,qB.w);

	rrDXT1EndPoints endpoints;
	endpoints.u.c0 = qA;
	endpoints.u.c1 = qB;
	*pEndPoints = endpoints.dw;
	
	return true;
}

bool DXT1_OptimizeEndPointsFromIndices_Inherit_Reindex(CpuDispatchFlags dispatch,rrDXT1Block * pBlock,U32 * pError, const rrColorBlock4x4 & colors, rrDXT1PaletteMode mode) //, rrDXTCOptions options)
{
	// keep previous fourc state :
	bool fourc = DXT1_Is4Color(*pBlock,mode);

	U32 endpoints = rrDXT1Block_EndPoints_AsU32(*pBlock);
	
	if ( ! DXT1_OptimizeEndPointsFromIndices_Raw(&endpoints,pBlock->indices,fourc,colors) )
	{
		return false;
	}

	// if endpoints didn't change, bail :
	if ( endpoints == rrDXT1Block_EndPoints_AsU32(*pBlock) )
	{
		return false;
	}
	
	// re-index for new endpoints :
	rrDXT1EndPoints ep; ep.dw = endpoints;
	rrColor32BGRA palette[4];
	DXT1_ComputePalette(ep.u.c0,ep.u.c1,palette,mode);
	
	U32 err;
	U32 indices = DXT1_FindIndices(dispatch,colors,palette,&err);
	if ( err < *pError )
	{
		// it got better, take it
		*pError = err;
		rrDXT1Block_EndPoints_AsU32(*pBlock) = endpoints;
		pBlock->indices = indices;
		return true;
	}
	else
	{
		return false;
	}
}

bool DXT1_OptimizeEndPointsFromIndices_Inherit_NoReindex(CpuDispatchFlags /*dispatch*/,rrDXT1Block * pBlock,U32 * pError, const rrColorBlock4x4 & colors, rrDXT1PaletteMode mode)
{
	// keep previous fourc state :
	bool fourc = DXT1_Is4Color(*pBlock,mode);

	U32 oldep = pBlock->endpoints;
	U32 endpoints = oldep;
	
	if ( ! DXT1_OptimizeEndPointsFromIndices_Raw(&endpoints,pBlock->indices,fourc,colors) )
	{
		return false;
	}

	// if endpoints didn't change, bail :
	if ( endpoints == oldep )
	{
		return false;
	}

	if ( mode == rrDXT1PaletteMode_Alpha )
	{
		// if we started out in four-color mode, new endpoints are in 3-color mode (probably
		// degenerate) and we have any pixels using index 3, we can't make the change;
		// we'd be not just turning that pixel black but also changing 1-bit transparency state.
		bool new_is_3c = ! DXT1_Is4Color(endpoints,mode);
		if ( fourc && DXT1_OneBitTransparent_Mask_FromIndices( new_is_3c, pBlock->indices) != 0 )
			return false;
	}
	
	// evaluate error with new endpoints :
	rrDXT1Block block;
	block.endpoints = endpoints;
	block.indices = pBlock->indices;
	
	// DXT1_OptimizeEndPointsFromIndices_Raw will try to preserve the fourc state
	//	except when it can't because endpoints were degenerate
	RR_ASSERT( DXT1_Is4Color(block,mode) == fourc || ( fourc && block.c0.w == block.c1.w ) );
	
	U32 err = DXT1_ComputeSSD_RGBA(colors,block,mode);
	if ( err < *pError )
	{
		// it got better, take it
		*pError = err;
		*pBlock = block;
		return true;
	}
	else
	{	
		return false;
	}
}

void DXT1_OptimizeEndPointsFromIndicesIterative(CpuDispatchFlags dispatch,rrDXT1Block * pBlock,U32 * pError, const rrColorBlock4x4 & colors,rrDXT1PaletteMode mode) //, rrDXTCOptions options)
{
	SIMPLEPROFILE_SCOPE(BC1_EndpointsFromIndsIter);
	
	for(;;)
	{
		U32 oldIndices = pBlock->indices;
		if ( ! DXT1_OptimizeEndPointsFromIndices_Inherit_Reindex(dispatch,pBlock,pError,colors,mode) )
			break;
		if ( oldIndices == pBlock->indices )
			break;
		// else indices changed so do it again
		
		// this almost never actually repeats
		//	it helps quality a tiny bit and doesn't hurt speed much
		
		// @@ while iterating does help a tiny bit, is it worth it speed-wise ?
	}
}

//=============================================================================================

// main external entry points :
			
void rrCompressDXT1Block(rrDXT1Block * pBlock,const rrColorBlock4x4 & colors, rrDXTCLevel level, rrDXTCOptions options, bool isBC23ColorBlock)
{
//	SIMPLEPROFILE_SCOPE(rrCompressDXT1Block);
	
	rrDXT1PaletteMode mode = (options & rrDXTCOptions_BC1_OneBitAlpha) ? rrDXT1PaletteMode_Alpha : rrDXT1PaletteMode_NoAlpha;
	if ( isBC23ColorBlock ) // BC2/3 (and also DXT3/5) color blocks don't support the 3-color mode and ignore endpoint ordering
		mode = rrDXT1PaletteMode_FourColor;
	
	// rrSurfaceDXTC_CompressBC1 does the canonicalize :
	RR_ASSERT( rrColorBlock4x4_IsBC1Canonical(colors,mode) );
	
	U32 err = RR_DXTC_INIT_ERROR;
		
	if ( level >= rrDXTCLevel_VerySlow )
		rrCompressDXT1_3( pBlock, &err, colors, options, mode, level );
	else if ( level == rrDXTCLevel_Slow )
		rrCompressDXT1_2( pBlock, &err, colors, options, mode );
	else if ( level == rrDXTCLevel_Fast )
		rrCompressDXT1_1( pBlock, &err, colors, options, mode );
	else // VeryFast
		rrCompressDXT1_0( pBlock, &err, colors, options, mode );

	// In BC2/3, both endpoint orderings result in four-color mode. So we have some freedom
	// here and want to pick a canonical choice.
	if ( mode == rrDXT1PaletteMode_FourColor )
	{
		rrDXT1Block_BC3_Canonicalize(pBlock);
	}
	else if ( mode == rrDXT1PaletteMode_Alpha )
	{
		RR_ASSERT( DXT1_OneBitTransparent_Same(colors,pBlock->endpoints,pBlock->indices) );
	}
}

//=============================================================================================

RR_NAMESPACE_END

