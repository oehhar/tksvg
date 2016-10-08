/*
//
// Copyright (c) 2013 Mikko Mononen memon@inside.org
//
// This software is provided 'as-is', without any express or implied
// warranty.  In no event will the authors be held liable for any damages
// arising from the use of this software.
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
// 1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software. If you use this software
//    in a product, an acknowledgment in the product documentation would be
//    appreciated but is not required.
// 2. Altered source versions must be plainly marked as such, and must not be
//    misrepresented as being the original software.
// 3. This notice may not be removed or altered from any source distribution.
//
*/

/* vim: set ts=8 sts=4 sw=4 : */
#include <stdio.h>
#include <string.h>
#include <float.h>
#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"

#include <tcl.h>
#include <tk.h>

static int		FileMatchSVG(Tcl_Channel chan, const char *fileName,
			    Tcl_Obj *format, int *widthPtr, int *heightPtr,
			    Tcl_Interp *interp);
static int		FileReadSVG(Tcl_Interp *interp, Tcl_Channel chan,
			    const char *fileName, Tcl_Obj *format,
			    Tk_PhotoHandle imageHandle, int destX, int destY,
			    int width, int height, int srcX, int srcY);
static int		StringMatchSVG(Tcl_Obj *dataObj, Tcl_Obj *format,
			    int *widthPtr, int *heightPtr, Tcl_Interp *interp);
static int		StringReadSVG(Tcl_Interp *interp, Tcl_Obj *dataObj,
			    Tcl_Obj *format, Tk_PhotoHandle imageHandle,
			    int destX, int destY, int width, int height,
			    int srcX, int srcY);

Tk_PhotoImageFormat tkImgFmtSVG = {
    "svg",			/* name */
    FileMatchSVG,		/* fileMatchProc */
    StringMatchSVG,		/* stringMatchProc */
    FileReadSVG,		/* fileReadProc */
    StringReadSVG,		/* stringReadProc */
    NULL,		/* fileWriteProc */
    NULL,		/* stringWriteProc */
    NULL
};

static NSVGimage * ParseSVGWithOptions(const char *input, size_t length, Tcl_Obj *format);

static int
FileMatchSVG(Tcl_Channel chan, const char *fileName,
			    Tcl_Obj *format, int *widthPtr, int *heightPtr,
			    Tcl_Interp *interp)
{
    Tcl_Obj *dataObj = Tcl_NewObj();
    if (Tcl_ReadChars(chan, dataObj, -1, 0) == -1) {
	/* in case of an error reading the file */
	Tcl_DecrRefCount(dataObj);
	return 0;
    }

    int result = StringMatchSVG(dataObj, format, widthPtr, heightPtr, interp);
    
    Tcl_DecrRefCount(dataObj);
    
    return result;
}

static int
FileReadSVG(Tcl_Interp *interp, Tcl_Channel chan,
			    const char *fileName, Tcl_Obj *format,
			    Tk_PhotoHandle imageHandle, int destX, int destY,
			    int width, int height, int srcX, int srcY)
{
    Tcl_Obj *dataObj = Tcl_NewObj();
    if (Tcl_ReadChars(chan, dataObj, -1, 0) == -1) {
	/* in case of an error reading the file */
	Tcl_DecrRefCount(dataObj);
	return 0;
    }

    int result = StringReadSVG(interp, dataObj, format, imageHandle, 
	destX, destY, width, height, srcX, srcY);
    
    Tcl_DecrRefCount(dataObj);
    
    return result;
}


static int
StringMatchSVG(Tcl_Obj *dataObj, Tcl_Obj *format,
			    int *widthPtr, int *heightPtr, Tcl_Interp *interp)
{

    int length;
    const char *data = Tcl_GetStringFromObj(dataObj, &length);
    NSVGimage* nsvgimage = ParseSVGWithOptions(data, length, format);
    if (nsvgimage != NULL) {
	/* Stupid interface: after parsing the file we destroy the parse 
	 * The image is parsed again on StringReadSVG */
	*widthPtr = nsvgimage -> width;
	*heightPtr = nsvgimage -> height;
	nsvgDelete(nsvgimage);
	return 1;
    }
    return 0;
}

static int
StringReadSVG(Tcl_Interp *interp, Tcl_Obj *dataObj,
			    Tcl_Obj *format, Tk_PhotoHandle imageHandle,
			    int destX, int destY, int width, int height,
			    int srcX, int srcY)
{   

    int length;
    const char *data = Tcl_GetStringFromObj(dataObj, &length);
    NSVGimage* nsvgimage = ParseSVGWithOptions(data, length, format);
    
    if (nsvgimage == NULL) {
	Tcl_SetResult(interp, "Cannot parse SVG image", TCL_STATIC);
	return TCL_ERROR;
    }

    
    int w = nsvgimage -> width;
    int h = nsvgimage -> height; 
    NSVGrasterizer *rast = nsvgCreateRasterizer();
   
    if (rast == NULL) {
	Tcl_SetResult(interp, "SVG: Could not initialize rasterizer", TCL_STATIC);
	goto cleanAST;
    }

    unsigned char *imgdata = ckalloc(w*h*4);
    if (imgdata == NULL) {
	Tcl_SetResult(interp, "Could not alloc image buffer", TCL_STATIC);
	goto cleanRAST;
    }

    nsvgRasterize(rast, nsvgimage, 0,0,1, imgdata, w, h, w*4);
    
    /* transfer the data to a photo block */
    Tk_PhotoImageBlock svgblock;
    svgblock.pixelPtr = imgdata;
    svgblock.width = w;
    svgblock.height = h;
    svgblock.pitch = w*4;
    svgblock.pixelSize = 4;
    for (int c=0; c<=3; c++) {
	svgblock.offset[c]=c;
    }

    if (Tk_PhotoExpand(interp, imageHandle,
	    destX + width, destY + height) != TCL_OK) {
	goto cleanRAST;
    }

    if (Tk_PhotoPutBlock(interp, imageHandle, &svgblock, destX, destY,
		width, height, TK_PHOTO_COMPOSITE_SET) != TCL_OK) {
	goto cleanimg;
    }


    ckfree(imgdata);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(nsvgimage);
    return TCL_OK;

cleanimg:
    ckfree(imgdata);
    
cleanRAST:
    nsvgDeleteRasterizer(rast);

cleanAST:
    nsvgDelete(nsvgimage);
    return TCL_ERROR;
}


static NSVGimage * ParseSVGWithOptions(const char *input, size_t length, Tcl_Obj *format) {
    /* The parser destroys the original input string, therfore first duplicate */
    char *input_dup = ckalloc(length+1);
    memcpy(input_dup, input, length);
    input_dup[length]='\0';
    
    /* here we should read the options (dpi) from format TODO */
    NSVGimage *nsvgi = nsvgParse(input_dup, "px", 96.0);
    ckfree(input_dup);
    
    return nsvgi;
}

int Tksvg_Init(Tcl_Interp* interp) {
    if (interp == 0) return TCL_ERROR;

    if (Tcl_InitStubs(interp, TCL_VERSION, 0) == NULL) {
	return TCL_ERROR;
    }

    if (Tk_InitStubs(interp, TCL_VERSION, 0) == NULL) {
	return TCL_ERROR;
    }

    Tk_CreatePhotoImageFormat(&tkImgFmtSVG);

    Tcl_PkgProvide(interp, PACKAGE_NAME, PACKAGE_VERSION);

    return TCL_OK;
}

