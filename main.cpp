#include <cmath>
#include <ctime>
#include <cstdio>
#include <cassert>
#include <vector>
#include <map>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H

#include "BinPacker.hpp"
#include "EncodingHelper.hpp"
#include "lodepng.h"
#include "stb_image.h"

using namespace std;

struct sdf_glyph
{
	int ID;
	int width, height;
	int x, y;
	float xoff, yoff;
	float xadv;
};

bool render_signed_distance_font(
		FT_Library &ft_lib,
		const char* font_file,
		const char* map_file,
		int texture_size,
		bool export_c_header );

bool render_signed_distance_image(
		const char* image_file,
		int texture_size,
		bool export_c_header );

unsigned char get_SDF_radial(
		unsigned char *fontmap,
		int w, int h,
		int x, int y,
		int max_radius );

bool gen_pack_list(
		FT_Face &ft_face,
		int pixel_size,
		int pack_tex_size,
		const std::vector< int > &render_list,
		std::vector< sdf_glyph > &packed_glyphs );

int save_png_SDFont(
		const char* orig_filename,
		const char* font_name,
		int img_width, int img_height,
		const std::vector< unsigned char > &img_data,
		std::vector< sdf_glyph > &packed_glyphs,
		const std::map<int, int> & char_map,
		int font_size );

int save_c_header_SDFont(
		const char* orig_filename,
		const char* font_name,
		int img_width, int img_height,
		const std::vector< unsigned char > &img_data,
		const std::vector< sdf_glyph > &packed_glyphs );

int map_char_id(
		int char_id, 
		FT_Encoding encoding );

int load_glyph( 
		FT_Face &ft_face,
		int char_id );

//	number of rendered pixels per SDF pixel
const int scaler = 16;
//	(larger value means higher quality, up to a point)

int main( int argc, char **argv )
{
	printf( "Signed Distance Bitmap Font Tool\n" );
	printf( "Jonathan \"lonesock\" Dummer\n" );
	printf( "\n" );
	if( argc < 2 )
	{
		printf( "usage: sdfont <fontfile.ttf>\n" );
		printf( "usage: sdfont <fontfile.ttf> <encoding.txt>\n" );
		printf( "usage: sdfont <fontfile.ttf> <encoding.txt> <size:64..4096>\n" );
		system( "pause" );
		return -1;
	}

	int texture_size = -1;	//	trigger a request
	bool export_c_header = false;

	if( argc >= 4 )
	{
		int argvSize = 0;
		if( sscanf( argv[3], "%i", &argvSize ) == 1 )
		{
			texture_size = argvSize;
		}
	}

	if( texture_size < 64 )
	{
		printf( "Select the texture size you would like for the output image.\n" );
		printf( "Your choice will be limited to the range 64 to 4096.\n" );
		printf( "Using powers of 2 is a good idea (e.g. 256 or 512).\n" );
		printf( "(note: negative values will also export a C header)\n\n" );
		printf( "Please select the texture size: " );
		scanf( "%i", &texture_size );
		printf( "\n" );
		if( texture_size < 0 )
		{
			texture_size = -texture_size;
			export_c_header = true;
		}
	}
	if( texture_size < 64 ) { texture_size = 64; }
	if( texture_size > 4096 ) { texture_size = 4096; }

	//	OK, try out FreeType2
	FT_Library ft_lib;
	int ft_err = FT_Init_FreeType( &ft_lib );
	if( ft_err )
	{
		printf( "Failed to initialize the FreeType library!\n" );
		system( "pause" );
		return -1;
	}

	//	this may be either an image, or a font file, try the image first
	if( !render_signed_distance_image( argv[1], texture_size, export_c_header ) )
	{
		//	didn't work, try the font
		const char * map_file = (argc >= 3) ? argv[2] : NULL;
		render_signed_distance_font( ft_lib, argv[1], map_file, texture_size, export_c_header );
	}

	ft_err = FT_Done_FreeType( ft_lib );

	//system( "pause" );
    return 0;
}

bool render_signed_distance_image(
		const char* image_file,
		int texture_size,
		bool export_c_header )
{
	//	try to load this file as an image
	int w, h, channels;
	unsigned char *img = stbi_load( image_file, &w, &h, &channels, 0 );
	if( !img )
	{
		return false;
	}
	//	image loaded
	printf( "Loaded '%s', %i x %i, channels 0", image_file, w, h );
	for( int i = 1; i < channels; ++i )
	{
		printf( ",%i", i );
	}
	printf( "\n" );
	//	check for components and resizing issues
	if( (w <= texture_size) && (h <= texture_size) )
	{
		printf( "The output texture size is larger than the input image dimensions!\n" );
		stbi_image_free( img );
		return false;
	}
	//	now, which channel do I use as the input function?
	int chan = 0;
	if( channels > 1 )
	{
		printf( "Which channel contains the input? " );
		scanf( "%i", &chan );
		if( chan < 0 )
		{
			chan = 0;
		} else if( chan >= channels )
		{
			chan = channels - 1;
		}
	}
	printf( "Using channel %i as the input\n", chan );
	std::vector<unsigned char> img_data;
	img_data.reserve( w*h );
	for( int i = chan; i < w*h*channels; i += channels )
	{
		img_data.push_back( img[i] );
	}
	stbi_image_free( img );
	//	is this channel strictly 2 values?
	bool needs_threshold = false;
	int vmax, vmin;
	{
		int val0 = img_data[0], val = -1;
		vmin = img_data[0];
		vmax = img_data[0];
		for( int i = 0; i < w*h; ++i )
		{
			//	do I need a threshold?
			if( img_data[i] != val0 )
			{
				if( val < 0 )
				{
					//	second value
					val = img_data[i];
				} else
				{
					needs_threshold = (val != img_data[i]);
				}
			}
			//	find min and max, just in case
			if( img_data[i] < vmin )
			{
				vmin = img_data[i];
			}
			if( img_data[i] > vmax )
			{
				vmax = img_data[i];
			}
		}
	}
	if( needs_threshold )
	{
		int thresh;
		printf( "The image needs a threshold, between %i and %i (< threshold is 0): ", vmin, vmax );
		scanf( "%i", &thresh );
		if( thresh <= vmin )
		{
			thresh = vmin + 1;
		} else if( thresh > vmax )
		{
			thresh = vmax;
		}
		printf( "using threshold=%i\n", thresh );
		for( int i = 0; i < w*h; ++i )
		{
			if( img_data[i] < thresh )
			{
				img_data[i] = 0;
			} else
			{
				img_data[i] = 255;
			}
		}
	}

	//	OK, I'm finally ready to perform the SDF analysis
	int sw;
	if( w > h )
	{
		sw = 2 * w / texture_size;
	} else
	{
		sw = 2 * h / texture_size;
	}
	std::vector<unsigned char> pdata( 4 * texture_size * texture_size, 0 );
	img = &(img_data[0]);
	for( int j = 0; j < texture_size; ++j )
	{
		for( int i = 0; i < texture_size; ++i )
		{
			int sx = i * (w-1) / (texture_size-1);
			int sy = j * (h-1) / (texture_size-1);
			int pd_idx = (i+j*texture_size) * 4;
			pdata[pd_idx] =
				get_SDF_radial
						( img, w, h,
						sx, sy, sw );
			pdata[pd_idx+1] = pdata[pd_idx];
			pdata[pd_idx+2] = pdata[pd_idx];
			pdata[pd_idx+3] = pdata[pd_idx];
		}
	}

	//	save the image
	int fn_size = strlen( image_file ) + 100;
	char *fn = new char[ fn_size ];
	#if 0
	sprintf( fn, "%s_sdf.bmp", image_file );
	stbi_write_bmp( fn, texture_size, texture_size, 4, &pdata[0] );
	#endif
	sprintf( fn, "%s_sdf.png", image_file );
	printf( "'%s'\n", fn );
	LodePNG::Encoder encoder;
	encoder.addText("Comment", "Signed Distance Image: lonesock tools");
	encoder.getSettings().zlibsettings.windowSize = 512; //	faster, not much worse compression
	std::vector<unsigned char> buffer;
	int tin = clock();
	encoder.encode( buffer, pdata.empty() ? 0 : &pdata[0], texture_size, texture_size );
	LodePNG::saveFile( buffer, fn );
	tin = clock() - tin;

	return true;
}

bool render_signed_distance_font(
		FT_Library &ft_lib,
		const char* font_file,
		const char* map_file,
		int texture_size,
		bool export_c_header )
{
	std::map<int, int> char_map;
	std::vector<int> render_list;

	if( map_file != NULL )
	{
		printf( "Loading character mapping file: '%s'\n", map_file );
		FILE * f = NULL;
		f = fopen( map_file, "r" );
		if( f == NULL )
		{ 
			printf( "Failed to open: '%s'\n", map_file );
			return false;
		}

		char line[80];
		while( fgets(line, 80, f) != NULL)
		{
			if (line[0] != '0') continue;
			
			int c = 0, u = 0;
			if( sscanf(line, "%x\t%x", &c, &u) != 2 ) printf("skip: %s", line);
			else char_map[u] = c;
		}

		fclose(f);

		for(std::map<int, int>::const_iterator i = char_map.begin(), e = char_map.end(); i != e; ++i )
		{
			render_list.push_back( i->first );
		}
	}

	FT_Face ft_face;
	int ft_err = FT_New_Face( ft_lib, font_file, 0, &ft_face );
	if( ft_err )
	{
		printf( "Failed to read the font file '%s'\n", font_file );
		return false;
	}

	if (render_list.size() == 0)
	{
		printf( "Font to convert to a Signed Distance Field:\n%s\n\n", font_file );
		int max_unicode_char= 0;
		if( max_unicode_char < 1 )
		{
			printf( "Select the highest unicode character you wish to render.\n" );
			printf( "Any characters without glyphs in the font will be skipped.\n" );
			printf( "(Good values for ANSI text might be 128 or 255, while\n" );
			printf( "a good value for Unicode text might be 65535.)\n\n" );
			printf( "Please select the maximum character value: " );
			scanf( "%i", &max_unicode_char );
			printf( "\n" );
		}
		if( max_unicode_char < 1 ) { max_unicode_char = 1; }
		//	Try all characters up to a user selected value (it will auto-skip any without glyphs)
		for( int char_idx = 0; char_idx <= max_unicode_char; ++char_idx )
		{
			render_list.push_back( char_idx );
		}
	}

	//	find the perfect size
	printf( "\nDetermining ideal font pixel size: " );
	std::vector< sdf_glyph > all_glyphs;
	//	initial guess for the size of the Signed Distance Field font
	//	(intentionally low, the first trial will be at sz*2, so 8x8)
	int sz = 4;
	bool keep_going = true;
	while( keep_going )
	{
		sz <<= 1;
		printf( " %i", sz );
		keep_going = gen_pack_list( ft_face, sz, texture_size, render_list, all_glyphs );
	}
	int sz_step = sz >> 2;
	while( sz_step )
	{
		if( keep_going )
		{
			sz += sz_step;
		} else
		{
			sz -= sz_step;
		}
		printf( " %i", sz );
		sz_step >>= 1;
		keep_going = gen_pack_list( ft_face, sz, texture_size, render_list, all_glyphs );
	}
	//	just in case
	while( (!keep_going) && (sz > 1) )
	{
		--sz;
		printf( " %i", sz );
		keep_going = gen_pack_list( ft_face, sz, texture_size, render_list, all_glyphs );
	}
	printf( "\nResult = %i pixels\n", sz );

	if( !keep_going )
	{
		printf( "The data will not fit in a texture %i^2\n", texture_size );
		system( "pause" );
		return -1;
	}

	//	set up the RAM for the final rendering/compositing
	//	(use all four channels, so PNG compression is simple)
	std::vector<unsigned char> pdata( 4 * texture_size * texture_size, 0 );

	//	render all the glyphs individually
	printf( "\nRendering characters into a packed %i^2 image:\n", texture_size );
	int tin = clock();
	int packed_glyph_index = 0;
	for( unsigned int char_index = 0; char_index < render_list.size(); ++char_index )
	{
		int char_id = load_glyph( ft_face, render_list[char_index]);
		if( char_id < 0 )
		{
			continue;
		}
		
		int w = ft_face->glyph->bitmap.width;
		int h = ft_face->glyph->bitmap.rows;
		int p = ft_face->glyph->bitmap.pitch;

		//	oversize the holding buffer so I can smooth it!
		int sw = w + scaler * 8; // * 4;
		int sh = h + scaler * 8; // * 4;
		unsigned char smooth_buf[sw * sh];
		for( int i = 0; i < sw * sh; ++i )
		{
			smooth_buf[i] = 0;
		}

		//	copy the glyph into the buffer to be smoothed
		unsigned char * buf = ft_face->glyph->bitmap.buffer;
		for( int j = 0; j < h; ++j )
		{
			for( int i = 0; i < w; ++i )
			{
				int value = 255 * ((buf[j * p + (i>>3)] >> (7 - (i & 7))) & 1);
				smooth_buf[i + scaler*2 + (j + scaler*2) * sw] = value;
			}
		}

		//	do the SDF
		int sdfw = all_glyphs[packed_glyph_index].width;
		int sdfx = all_glyphs[packed_glyph_index].x;
		int sdfh = all_glyphs[packed_glyph_index].height;
		int sdfy = all_glyphs[packed_glyph_index].y;
		for( int j = 0; j < sdfh; ++j )
		{
			for( int i = 0; i < sdfw; ++i )
			{
				int pd_idx = (i+sdfx+(j+sdfy)*texture_size) * 4;
				pdata[pd_idx] =
					//get_SDF
					get_SDF_radial
							( smooth_buf, sw, sh,
							i*scaler + (scaler/2), j*scaler + (scaler/2),
							2*scaler );
				pdata[pd_idx+1] = pdata[pd_idx];
				pdata[pd_idx+2] = pdata[pd_idx];
				pdata[pd_idx+3] = pdata[pd_idx];
			}
		}
		++packed_glyph_index;
	}
	tin = clock() - tin;
	printf( "\nRenderint took %1.3f seconds\n\n", 0.001f * tin );

	printf( "\nCompressing the image to PNG\n" );
	tin = save_png_SDFont(
			font_file, ft_face->family_name,
			texture_size, texture_size,
			pdata, all_glyphs, char_map, sz );
	printf( "Done in %1.3f seconds\n\n", 0.001f * tin );

	if( export_c_header )
	{
		printf( "Saving the SDF data in a C header file\n" );
		tin = save_c_header_SDFont(
				font_file, ft_face->family_name,
				texture_size, texture_size,
				pdata, all_glyphs );
		printf( "Done in %1.3f seconds\n\n", 0.001f * tin );
	}

	//	clean up my data
	all_glyphs.clear();
	pdata.clear();
	ft_err = FT_Done_Face( ft_face );
	
	return true;
}

int save_png_SDFont(
		const char* orig_filename,
		const char* font_name,
		int img_width, int img_height,
		const std::vector< unsigned char > &img_data,
		std::vector< sdf_glyph > &packed_glyphs,
		const std::map<int, int> & char_map,
		int font_size )
{
	//	save my image
	int fn_size = strlen( orig_filename ) + 100;
	char *fn = new char[ fn_size ];
	sprintf( fn, "%s_sdf.png", orig_filename );
	printf( "'%s'\n", fn );
	LodePNG::Encoder encoder;
	encoder.addText("Comment", "Signed Distance Font: lonesock tools");
	encoder.getSettings().zlibsettings.windowSize = 512; //	faster, not much worse compression
	std::vector<unsigned char> buffer;
	int tin = clock();
	encoder.encode( buffer, img_data.empty() ? 0 : &img_data[0], img_width, img_height );
	LodePNG::saveFile( buffer, fn );
	tin = clock() - tin;
	
	// remap from unicode to codepage, get font height
	float ymax = 0, ymin = 0;
	if( char_map.size() != 0 )
	{
		for( unsigned int i = 0; i < packed_glyphs.size(); ++i )
		{
			std::map<int, int>::const_iterator id;
			if( (id = char_map.find(packed_glyphs[i].ID)) != char_map.end() )
			{
				packed_glyphs[i].ID = id->second;
				float ymaxi = packed_glyphs[i].yoff;
				float ymini = packed_glyphs[i].yoff - packed_glyphs[i].height;
				if (ymax < ymaxi) ymax = ymaxi;
				if (ymin > ymini) ymin = ymini;
			}
		}
	}

	//	now save the acompanying info
	sprintf( fn, "%s_sdf.txt", orig_filename );
	FILE *fp = fopen( fn, "w" );
	if( fp )
	{
		fprintf( fp, "info face=\"%s\"\n", font_name  );
		fprintf( fp, "size=%i\n", font_size );
		fprintf( fp, "ascent=%2.0f\n", ymax );
		fprintf( fp, "descent=%2.0f\n", ymin );
		fprintf( fp, "chars count=%zu\n", packed_glyphs.size() );
		for( unsigned int i = 0; i < packed_glyphs.size(); ++i )
		{		
			fprintf( fp, "char id=%-6ix=%-6iy=%-6iwidth=%-6iheight=%-6i",
				packed_glyphs[i].ID,
				packed_glyphs[i].x,
				packed_glyphs[i].y,
				packed_glyphs[i].width,
				packed_glyphs[i].height );
			
			fprintf( fp, "xoffset=%-10.3fyoffset=%-10.3fxadvance=%-10.3f",
				packed_glyphs[i].xoff,
				packed_glyphs[i].yoff,
				packed_glyphs[i].xadv );
			
			fprintf( fp, "  page=0  chnl=0\n" );
		}
		fclose( fp );
	}
	delete [] fn;
	return tin;
}

int save_c_header_SDFont(
		const char* orig_filename,
		const char* font_name,
		int img_width, int img_height,
		const std::vector< unsigned char > &img_data,
		const std::vector< sdf_glyph > &packed_glyphs )
{
	//	save my image
	int fn_size = strlen( orig_filename ) + strlen( font_name ) + 100;
	char *fn = new char[ fn_size ];
	int tin = clock();

	//	now save the acompanying info
	sprintf( fn, "%s_sdf.h", orig_filename );
	FILE *fp = fopen( fn, "w" );
	if( fp )
	{
		fprintf( fp, "/*\n" );
		fprintf( fp, "    Jonathan \"lonesock\" Dummer\n" );
		fprintf( fp, "    Signed Distance Font Tool\n" );
		fprintf( fp, "\n" );
		fprintf( fp, "    C header\n" );
		fprintf( fp, "    font: \"%s\"\n", font_name );
		fprintf( fp, "*/\n" );
		fprintf( fp, "\n" );
		fprintf( fp, "#ifndef HEADER_SIGNED_DISTANCE_FONT_XXX\n" );
		fprintf( fp, "#define HEADER_SIGNED_DISTANCE_FONT_XXX\n" );
		fprintf( fp, "\n" );
		fprintf( fp, "/* array size information */\n" );
		fprintf( fp, "const int sdf_tex_width = %i;\n", img_width );
		fprintf( fp, "const int sdf_tex_height = %i;\n", img_height );
		fprintf( fp, "const int sdf_num_chars = %zu;\n", packed_glyphs.size() );
		fprintf( fp, "/* 'unsigned char sdf_data[]' is defined last */\n" );
		fprintf( fp, "\n" );

		//	now give the glyph spacing info
		fprintf( fp, "/*\n" );
		fprintf( fp, "    The following array holds the spacing info for rendering.\n" );
		fprintf( fp, "    Note that the final 3 values need sub-pixel accuracy, so\n" );
		fprintf( fp, "    they are multiplied by a scaling factor.  Make sure to\n" );
		fprintf( fp, "    divide by scale_factor before using the 'offset' and\n" );
		fprintf( fp, "    'advance' values.\n" );
		fprintf( fp, "\n" );
		fprintf( fp, "    Here is the data order in the following array:\n" );
		fprintf( fp, "    [0] Unicode character ID\n" );
		fprintf( fp, "    [1] X position in this texture\n" );
		fprintf( fp, "    [2] Y position in this texture\n" );
		fprintf( fp, "    [3] Width of this glyph in the texture\n" );
		fprintf( fp, "    [4] Height of this glyph in the texture\n" );
		fprintf( fp, "    [5] X Offset * scale_factor  | Draw the glyph at X,Y offset\n" );
		fprintf( fp, "    [6] Y Offset * scale_factor  | relative to the cursor, then\n" );
		fprintf( fp, "    [7] X Advance * scale_factor | advance the cursor by this.\n" );
		fprintf( fp, "*/\n" );
		const float scale_factor = 1000.0;
		fprintf( fp, "const float scale_factor = %f;\n", scale_factor );
		fprintf( fp, "const int sdf_spacing[] = {\n" );
		for( unsigned int i = 0; i < packed_glyphs.size(); ++i )
		{
			fprintf( fp, "  %i,%i,%i,%i,%i,",
				packed_glyphs[i].ID,
				packed_glyphs[i].x,
				packed_glyphs[i].y,
				packed_glyphs[i].width,
				packed_glyphs[i].height
				);
			fprintf( fp, "%i,%i,%i,\n",
				(int)(scale_factor * packed_glyphs[i].xoff),
				(int)(scale_factor * packed_glyphs[i].yoff),
				(int)(scale_factor * packed_glyphs[i].xadv)
				);
		}
		fprintf( fp, "  0\n};\n\n" );

		fprintf( fp, "/* Signed Distance Field: edges are at 127.5 */\n" );
		fprintf( fp, "const unsigned char sdf_data[] = {" );
		int nchars = 100000;
		for( unsigned int i = 0; i < img_data.size(); i += 4 )
		{
			if( nchars > 70 )
			{
				fprintf( fp, "\n  " );
				nchars = 2;
			}
			//	print the value
			int v = img_data[i];
			fprintf( fp, "%i,", v );
			//	account for the comma
			++nchars;
			//	account for the number
			if( v > 99 )
			{
				nchars += 3;
			} else if( v > 9 )
			{
				nchars += 2;
			} else
			{
				++nchars;
			}
		}
		//	an ending value
		fprintf( fp, "\n  255\n};\n\n" );

		fprintf( fp, "#endif /* HEADER_SIGNED_DISTANCE_FONT_XXX */\n" );
		fclose( fp );
	}
	delete [] fn;
	tin = clock() - tin;

	return tin;
}

int map_char_id(
		int char_id, 
		FT_Encoding encoding )
{
	if( encoding == FT_ENCODING_APPLE_ROMAN )
	{
		return unicode_to_apple_roman(char_id);
	}

	return char_id;
}

int load_glyph( 
		FT_Face &ft_face,
		int char_id )
{
	for( int i = 0; i < ft_face->num_charmaps; ++i )
	{
		int mapped_char_id = map_char_id( char_id, ft_face->charmap->encoding );
		int glyph_index = FT_Get_Char_Index( ft_face, mapped_char_id );
		if( glyph_index == 0 ||
			FT_Load_Glyph( ft_face, glyph_index, 0 ) || 
			FT_Render_Glyph( ft_face->glyph, FT_RENDER_MODE_MONO ) )
		{
			int charmap_index = FT_Get_Charmap_Index( ft_face->charmap );
			charmap_index = ( charmap_index + 1 ) % ft_face->num_charmaps;
			FT_Set_Charmap( ft_face, ft_face->charmaps[charmap_index] );
			continue;
		}
		else
		{
			return mapped_char_id;
		}
	}

	printf("Failed loading glyph: 0x%x\n", char_id);
	return -1;
}


bool gen_pack_list(
		FT_Face &ft_face,
		int pixel_size,
		int pack_tex_size,
		const std::vector< int > &render_list,
		std::vector< sdf_glyph > &packed_glyphs )
{
	int ft_err;
	packed_glyphs.clear();
	ft_err = FT_Set_Pixel_Sizes( ft_face, pixel_size * scaler, 0 );

	std::vector< int > rectangle_info;
	std::vector< std::vector<int> > packed_glyph_info;
	for( unsigned int char_index = 0; char_index < render_list.size(); ++char_index )
	{
		int char_id = load_glyph(ft_face, render_list[char_index]);
		if( char_id < 0 )
		{
			continue;
		}

		sdf_glyph add_me;
		//	we have the glyph, already rendered, get the data about it
		int w = ft_face->glyph->bitmap.width;
		int h = ft_face->glyph->bitmap.rows;
		//	oversize the holding buffer so I can smooth it!
		int sw = w + scaler * 8; // * 4;
		int sh = h + scaler * 8; // * 4
		//	do the SDF
		int sdfw = sw / scaler;
		int sdfh = sh / scaler;
		rectangle_info.push_back( sdfw );
		rectangle_info.push_back( sdfh );
		//	add in the data I already know
		add_me.ID = render_list[char_index];
		add_me.width = sdfw;
		add_me.height = sdfh;
		//	these need to be filled in later (after packing)
		add_me.x = -1;
		add_me.y = -1;
		//	these need scaling...
		add_me.xoff = ft_face->glyph->bitmap_left;
		add_me.yoff = ft_face->glyph->bitmap_top;
		add_me.xadv = ft_face->glyph->advance.x / 64.0;
		//	so scale them (the 1.5's have to do with the padding
		//	border and the sampling locations for the SDF)
		add_me.xoff = add_me.xoff / scaler - 3; // - 1.5;
		add_me.yoff = add_me.yoff / scaler + 3; // + 1.5;
		add_me.xadv = add_me.xadv / scaler;
		//	add it to my list
		packed_glyphs.push_back( add_me );
	}
	
	const bool dont_allow_rotation = false;
	BinPacker bp;
	bp.Pack( rectangle_info, packed_glyph_info, pack_tex_size, dont_allow_rotation );
	//	populate the actual coordinates
	if( packed_glyph_info.size() == 1 )
	{
		//	it all fit into one!
		unsigned int lim = packed_glyph_info[0].size();
		for( unsigned int i = 0; i < lim; i += 4 )
		{
			//	index, x, y, rotated
			unsigned int idx = packed_glyph_info[0][i+0];
			packed_glyphs[idx].x = packed_glyph_info[0][i+1];
			packed_glyphs[idx].y = packed_glyph_info[0][i+2];
		}
		return true;
	}
	return false;
}

unsigned char get_SDF_radial(
		unsigned char *fontmap,
		int w, int h,
		int x, int y,
		int max_radius )
{
	//	hideous brute force method
	float d2 = max_radius*max_radius+1.0;
	unsigned char v = fontmap[x+y*w];
	for( int radius = 1; (radius <= max_radius) && (radius*radius < d2); ++radius )
	{
		int line, lo, hi;
		//	north
		line = y - radius;
		if( (line >= 0) && (line < h) )
		{
			lo = x - radius;
			hi = x + radius;
			if( lo < 0 ) { lo = 0; }
			if( hi >= w ) { hi = w-1; }
			int idx = line * w + lo;
			for( int i = lo; i <= hi; ++i )
			{
				//	check this pixel
				if( fontmap[idx] != v )
				{
					float nx = i - x;
					float ny = line - y;
					float nd2 = nx*nx+ny*ny;
					if( nd2 < d2 )
					{
						d2 = nd2;
					}
				}
				//	move on
				++idx;
			}
		}
		//	south
		line = y + radius;
		if( (line >= 0) && (line < h) )
		{
			lo = x - radius;
			hi = x + radius;
			if( lo < 0 ) { lo = 0; }
			if( hi >= w ) { hi = w-1; }
			int idx = line * w + lo;
			for( int i = lo; i <= hi; ++i )
			{
				//	check this pixel
				if( fontmap[idx] != v )
				{
					float nx = i - x;
					float ny = line - y;
					float nd2 = nx*nx+ny*ny;
					if( nd2 < d2 )
					{
						d2 = nd2;
					}
				}
				//	move on
				++idx;
			}
		}
		//	west
		line = x - radius;
		if( (line >= 0) && (line < w) )
		{
			lo = y - radius + 1;
			hi = y + radius - 1;
			if( lo < 0 ) { lo = 0; }
			if( hi >= h ) { hi = h-1; }
			int idx = lo * w + line;
			for( int i = lo; i <= hi; ++i )
			{
				//	check this pixel
				if( fontmap[idx] != v )
				{
					float nx = line - x;
					float ny = i - y;
					float nd2 = nx*nx+ny*ny;
					if( nd2 < d2 )
					{
						d2 = nd2;
					}
				}
				//	move on
				idx += w;
			}
		}
		//	east
		line = x + radius;
		if( (line >= 0) && (line < w) )
		{
			lo = y - radius + 1;
			hi = y + radius - 1;
			if( lo < 0 ) { lo = 0; }
			if( hi >= h ) { hi = h-1; }
			int idx = lo * w + line;
			for( int i = lo; i <= hi; ++i )
			{
				//	check this pixel
				if( fontmap[idx] != v )
				{
					float nx = line - x;
					float ny = i - y;
					float nd2 = nx*nx+ny*ny;
					if( nd2 < d2 )
					{
						d2 = nd2;
					}
				}
				//	move on
				idx += w;
			}
		}
	}
	d2 = sqrtf( d2 );
	if( v==0 )
	{
		d2 = -d2;
	}
	d2 *= 127.5 / max_radius;
	d2 += 127.5;
	if( d2 < 0.0 ) d2 = 0.0;
	if( d2 > 255.0 ) d2 = 255.0;
	return (unsigned char)(d2 + 0.5);
}
