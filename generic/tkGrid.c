/*
 * tkGrid.c --
 *
 *	Grid based geometry manager.
 *
 * Copyright © 1996-1997 Sun Microsystems, Inc.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include "tkInt.h"

/*
 * Convenience Macros
 */

#ifdef MAX
#   undef MAX
#endif
#define MAX(x,y)	((x) > (y) ? (x) : (y))

#define COLUMN		(1)	/* Working on column offsets. */
#define ROW		(2)	/* Working on row offsets. */

#define CHECK_ONLY	(1)	/* Check max slot constraint. */
#define CHECK_SPACE	(2)	/* Alloc more space, don't change max. */

/*
 * Pre-allocate enough row and column slots for "typical" sized tables this
 * value should be chosen so by the time the extra malloc's are required, the
 * layout calculations overwehlm them. [A "slot" contains information for
 * either a row or column, depending upon the context.]
 */

#define TYPICAL_SIZE	25	/* (Arbitrary guess) */
#define PREALLOC	10	/* Extra slots to allocate. */

/*
 * Pre-allocate room for uniform groups during layout.
 */

#define UNIFORM_PREALLOC 10

/*
 * Data structures are allocated dynamically to support arbitrary sized
 * tables. However, the space is proportional to the highest numbered slot
 * with some non-default property. This limit is used to head off mistakes and
 * denial of service attacks by limiting the amount of storage required.
 */

#define MAX_ELEMENT	10000

/*
 * Special characters to support relative layouts.
 */

#define REL_SKIP	'x'	/* Skip this column. */
#define REL_HORIZ	'-'	/* Extend previous widget horizontally. */
#define REL_VERT	'^'	/* Extend widget from row above. */

/*
 * Default value for 'grid anchor'.
 */

#define GRID_DEFAULT_ANCHOR TK_ANCHOR_NW

/*
 * Structure to hold information for grid containers. A slot is either a row or
 * column.
 */

typedef struct SlotInfo {
    int minSize;		/* The minimum size of this slot (in pixels).
				 * It is set via the rowconfigure or
				 * columnconfigure commands. */
    int weight;			/* The resize weight of this slot. (0) means
				 * this slot doesn't resize. Extra space in
				 * the layout is given distributed among slots
				 * inproportion to their weights. */
    int pad;			/* Extra padding, in pixels, required for this
				 * slot. This amount is "added" to the largest
				 * content in the slot. */
    Tk_Uid uniform;		/* Value of -uniform option. It is used to
				 * group slots that should have the same
				 * size. */
    int offset;			/* This is a cached value used for
				 * introspection. It is the pixel offset of
				 * the right or bottom edge of this slot from
				 * the beginning of the layout. */
    int temp;			/* This is a temporary value used for
				 * calculating adjusted weights when shrinking
				 * the layout below its nominal size. */
} SlotInfo;

/*
 * Structure to hold information during layout calculations. There is one of
 * these for each slot, an array for each of the rows or columns.
 */

typedef struct GridLayout {
    struct Gridder *binNextPtr;	/* The next content window in this bin. Each bin
				 * contains a list of all content whose spans
				 * are >1 and whose right edges fall in this
				 * slot. */
    int minSize;		/* Minimum size needed for this slot, in
				 * pixels. This is the space required to hold
				 * any content contained entirely in this slot,
				 * adjusted for any slot constrants, such as
				 * size or padding. */
    int pad;			/* Padding needed for this slot */
    int weight;			/* Slot weight, controls resizing. */
    Tk_Uid uniform;		/* Value of -uniform option. It is used to
				 * group slots that should have the same
				 * size. */
    int minOffset;		/* The minimum offset, in pixels, from the
				 * beginning of the layout to the bottom/right
				 * edge of the slot calculated from top/left
				 * to bottom/right. */
    int maxOffset;		/* The maximum offset, in pixels, from the
				 * beginning of the layout to the bottom/right
				 * edge of the slot calculated from
				 * bottom/right to top/left. */
} GridLayout;

/*
 * Keep one of these for each geometry container.
 */

typedef struct {
    SlotInfo *columnPtr;	/* Pointer to array of column constraints. */
    SlotInfo *rowPtr;		/* Pointer to array of row constraints. */
    int columnEnd;		/* The last column occupied by any content. */
    int columnMax;		/* The number of columns with constraints. */
    int columnSpace;		/* The number of slots currently allocated for
				 * column constraints. */
    int rowEnd;			/* The last row occupied by any content. */
    int rowMax;			/* The number of rows with constraints. */
    int rowSpace;		/* The number of slots currently allocated for
				 * row constraints. */
    int startX;			/* Pixel offset of this layout within its
				 * container. */
    int startY;			/* Pixel offset of this layout within its
				 * container. */
    Tk_Anchor anchor;		/* Value of anchor option: specifies where a
				 * grid without weight should be placed. */
} GridContainer;

/*
 * For each window that the grid cares about (either because the window is
 * managed by the grid or because the window has content that are managed by
 * the grid), there is a structure of the following type:
 */

typedef struct Gridder {
    Tk_Window tkwin;		/* Tk token for window. NULL means that the
				 * window has been deleted, but the gridder
				 * hasn't had a chance to clean up yet because
				 * the structure is still in use. */
    struct Gridder *containerPtr;	/* Container window within which this window is
				 * managed (NULL means this window isn't
				 * managed by the gridder). */
    struct Gridder *nextPtr;	/* Next window managed within same container.
				 * List order doesn't matter. */
    struct Gridder *contentPtr;	/* First in list of content managed inside this
				 * window (NULL means no grid content). */
    GridContainer *containerDataPtr;	/* Additional data for geometry container. */
    Tcl_Obj *in;                /* Store container name when removed. */
    int column, row;		/* Location in the grid (starting from
				 * zero). */
    int numCols, numRows;	/* Number of columns or rows this content spans.
				 * Should be at least 1. */
    int padX, padY;		/* Total additional pixels to leave around the
				 * window. Some is of this space is on each
				 * side. This is space *outside* the window:
				 * we'll allocate extra space in frame but
				 * won't enlarge window). */
    int padLeft, padTop;	/* The part of padX or padY to use on the left
				 * or top of the widget, respectively. By
				 * default, this is half of padX or padY. */
    int iPadX, iPadY;		/* Total extra pixels to allocate inside the
				 * window (half this amount will appear on
				 * each side). */
    int sticky;			/* which sides of its cavity this window
				 * sticks to. See below for definitions */
    int doubleBw;		/* Twice the window's last known border width.
				 * If this changes, the window must be
				 * re-arranged within its container. */
    int *abortPtr;		/* If non-NULL, it means that there is a
				 * nested call to ArrangeGrid already working
				 * on this window. *abortPtr may be set to 1
				 * to abort that nested call. This happens,
				 * for example, if tkwin or any of its content
				 * is deleted. */
    int flags;			/* Miscellaneous flags; see below for
				 * definitions. */

    /*
     * These fields are used temporarily for layout calculations only.
     */

    struct Gridder *binNextPtr;	/* Link to next span>1 content in this bin. */
    int size;			/* Nominal size (width or height) in pixels of
				 * the content. This includes the padding. */
} Gridder;

/*
 * Flag values for "sticky"ness. The 16 combinations subsume the packer's
 * notion of anchor and fill.
 *
 * STICK_NORTH			This window sticks to the top of its cavity.
 * STICK_EAST			This window sticks to the right edge of its
 *				cavity.
 * STICK_SOUTH			This window sticks to the bottom of its cavity.
 * STICK_WEST			This window sticks to the left edge of its
 *				cavity.
 */

#define STICK_NORTH		1
#define STICK_EAST		2
#define STICK_SOUTH		4
#define STICK_WEST		8


/*
 * Structure to gather information about uniform groups during layout.
 */

typedef struct UniformGroup {
    Tk_Uid group;
    int minSize;
} UniformGroup;

/*
 * Flag values for Grid structures:
 *
 * REQUESTED_RELAYOUT		1 means a Tcl_DoWhenIdle request has already
 *				been made to re-arrange all the content of this
 *				window.
 * DONT_PROPAGATE		1 means don't set this window's requested
 *				size. 0 means if this window is a container then
 *				Tk will set its requested size to fit the
 *				needs of its content.
 * ALLOCED_CONTAINER		1 means that Grid has allocated itself as
 *				geometry container for this window.
 */

#define REQUESTED_RELAYOUT	1
#define DONT_PROPAGATE		2
#define ALLOCED_CONTAINER	4

/*
 * Prototypes for procedures used only in this file:
 */

static void		AdjustForSticky(Gridder *contentPtr, int *xPtr,
			    int *yPtr, int *widthPtr, int *heightPtr);
static int		AdjustOffsets(int width, Tcl_Size elements,
			    SlotInfo *slotPtr);
static void		ArrangeGrid(void *clientData);
static int		CheckSlotData(Gridder *containerPtr, Tcl_Size slot,
			    int slotType, int checkOnly);
static int		ConfigureContent(Tcl_Interp *interp, Tk_Window tkwin,
			    Tcl_Size objc, Tcl_Obj *const objv[]);
static Tcl_FreeProc	DestroyGrid;
static Gridder *	GetGrid(Tk_Window tkwin);
static int		GridAnchorCommand(Tk_Window tkwin, Tcl_Interp *interp,
			    Tcl_Size objc, Tcl_Obj *const objv[]);
static int		GridBboxCommand(Tk_Window tkwin, Tcl_Interp *interp,
			    Tcl_Size objc, Tcl_Obj *const objv[]);
static int		GridForgetRemoveCommand(Tk_Window tkwin,
			    Tcl_Interp *interp, Tcl_Size objc,
			    Tcl_Obj *const objv[]);
static int		GridInfoCommand(Tk_Window tkwin, Tcl_Interp *interp,
			    Tcl_Size objc, Tcl_Obj *const objv[]);
static int		GridLocationCommand(Tk_Window tkwin,
			    Tcl_Interp *interp, Tcl_Size objc,
			    Tcl_Obj *const objv[]);
static int		GridPropagateCommand(Tk_Window tkwin,
			    Tcl_Interp *interp, Tcl_Size objc,
			    Tcl_Obj *const objv[]);
static int		GridRowColumnConfigureCommand(Tk_Window tkwin,
			    Tcl_Interp *interp, Tcl_Size objc,
			    Tcl_Obj *const objv[]);
static int		GridSizeCommand(Tk_Window tkwin, Tcl_Interp *interp,
			    Tcl_Size objc, Tcl_Obj *const objv[]);
static int		GridContentCommand(Tk_Window tkwin, Tcl_Interp *interp,
			    Tcl_Size objc, Tcl_Obj *const objv[]);
static void		GridStructureProc(void *clientData,
			    XEvent *eventPtr);
static void		GridLostContentProc(void *clientData,
			    Tk_Window tkwin);
static void		GridReqProc(void *clientData, Tk_Window tkwin);
static void		InitContainerData(Gridder *containerPtr);
static Tcl_Obj *	NewPairObj(Tcl_WideInt, Tcl_WideInt);
static Tcl_Obj *	NewQuadObj(Tcl_WideInt, Tcl_WideInt, Tcl_WideInt, Tcl_WideInt);
static int		ResolveConstraints(Gridder *gridPtr, int rowOrColumn,
			    int maxOffset);
static void		SetGridSize(Gridder *gridPtr);
static int		SetContentColumn(Tcl_Interp *interp, Gridder *contentPtr,
			    int column, int numCols);
static int		SetContentRow(Tcl_Interp *interp, Gridder *contentPtr,
			    int row, int numRows);
static Tcl_Obj *	StickyToObj(int flags);
static int		StringToSticky(const char *string);
static void		Unlink(Gridder *gridPtr);

static const Tk_GeomMgr gridMgrType = {
    "grid",			/* name */
    GridReqProc,		/* requestProc */
    GridLostContentProc,		/* lostContentProc */
};

/*
 *----------------------------------------------------------------------
 *
 * Tk_GridCmd --
 *
 *	This procedure is invoked to process the "grid" Tcl command. See the
 *	user documentation for details on what it does.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	See the user documentation.
 *
 *----------------------------------------------------------------------
 */

int
Tk_GridObjCmd(
    void *clientData,	/* Main window associated with interpreter. */
    Tcl_Interp *interp,		/* Current interpreter. */
    int objc,			/* Number of arguments. */
    Tcl_Obj *const objv[])	/* Argument objects. */
{
    Tk_Window tkwin = (Tk_Window)clientData;
    static const char *const optionStrings[] = {
	"anchor", "bbox", "columnconfigure", "configure",
	"content", "forget", "info", "location", "propagate",
	"remove", "rowconfigure", "size", "slaves", NULL
    };
    static const char *const optionStringsNoDep[] = {
	"anchor", "bbox", "columnconfigure", "configure",
	"content", "forget", "info", "location", "propagate",
	"remove", "rowconfigure", "size", NULL
    };
    enum options {
	GRID_ANCHOR, GRID_BBOX, GRID_COLUMNCONFIGURE, GRID_CONFIGURE,
	GRID_CONTENT, GRID_FORGET, GRID_INFO, GRID_LOCATION, GRID_PROPAGATE,
	GRID_REMOVE, GRID_ROWCONFIGURE, GRID_SIZE, GRID_SLAVES
    };
    int index;

    if (objc >= 2) {
	const char *argv1 = Tcl_GetString(objv[1]);

	if ((argv1[0] == '.') || (argv1[0] == REL_SKIP) ||
		(argv1[0] == REL_VERT)) {
	    return ConfigureContent(interp, tkwin, objc-1, objv+1);
	}
    }
    if (objc < 3) {
	Tcl_WrongNumArgs(interp, 1, objv, "option arg ?arg ...?");
	return TCL_ERROR;
    }

    if (Tcl_GetIndexFromObjStruct(NULL, objv[1], optionStrings,
	    sizeof(char *), "option", 0, &index) != TCL_OK) {
	/*
	 * Call it again without the deprecated ones to get a proper error
	 * message. This works well since there can't be any ambiguity between
	 * deprecated and new options.
	 */

	Tcl_GetIndexFromObjStruct(interp, objv[1], optionStringsNoDep,
		sizeof(char *), "option", 0, &index);
	return TCL_ERROR;
    }

    switch ((enum options) index) {
    case GRID_ANCHOR:
	return GridAnchorCommand(tkwin, interp, objc, objv);
    case GRID_BBOX:
	return GridBboxCommand(tkwin, interp, objc, objv);
    case GRID_CONFIGURE:
	return ConfigureContent(interp, tkwin, objc-2, objv+2);
    case GRID_FORGET:
    case GRID_REMOVE:
	return GridForgetRemoveCommand(tkwin, interp, objc, objv);
    case GRID_INFO:
	return GridInfoCommand(tkwin, interp, objc, objv);
    case GRID_LOCATION:
	return GridLocationCommand(tkwin, interp, objc, objv);
    case GRID_PROPAGATE:
	return GridPropagateCommand(tkwin, interp, objc, objv);
    case GRID_SIZE:
	return GridSizeCommand(tkwin, interp, objc, objv);
    case GRID_SLAVES:
    case GRID_CONTENT:
	return GridContentCommand(tkwin, interp, objc, objv);

    /*
     * Sample argument combinations:
     *  grid columnconfigure <container> <index> -option
     *  grid columnconfigure <container> <index> -option value -option value
     *  grid rowconfigure <container> <index>
     *  grid rowconfigure <container> <index> -option
     *  grid rowconfigure <container> <index> -option value -option value.
     */

    case GRID_COLUMNCONFIGURE:
    case GRID_ROWCONFIGURE:
	return GridRowColumnConfigureCommand(tkwin, interp, objc, objv);
    }

    /* This should not happen */
    Tcl_SetObjResult(interp, Tcl_NewStringObj("internal error in grid", TCL_INDEX_NONE));
    Tcl_SetErrorCode(interp, "TK", "API_ABUSE", (char *)NULL);
    return TCL_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * GridAnchorCommand --
 *
 *	Implementation of the [grid anchor] subcommand. See the user
 *	documentation for details on what it does.
 *
 * Results:
 *	Standard Tcl result.
 *
 * Side effects:
 *	May recompute grid geometry.
 *
 *----------------------------------------------------------------------
 */

static int
GridAnchorCommand(
    Tk_Window tkwin,		/* Main window of the application. */
    Tcl_Interp *interp,		/* Current interpreter. */
    Tcl_Size objc,			/* Number of arguments. */
    Tcl_Obj *const objv[])	/* Argument objects. */
{
    Tk_Window container;
    Gridder *containerPtr;
    GridContainer *gridPtr;
    Tk_Anchor old;

    if (objc > 4) {
	Tcl_WrongNumArgs(interp, 2, objv, "window ?anchor?");
	return TCL_ERROR;
    }

    if (TkGetWindowFromObj(interp, tkwin, objv[2], &container) != TCL_OK) {
	return TCL_ERROR;
    }
    if (!(containerPtr = GetGrid(container))) {
	return TCL_OK;
    }

    if (objc == 3) {
	gridPtr = containerPtr->containerDataPtr;
	Tcl_SetObjResult(interp, Tcl_NewStringObj(
		Tk_NameOfAnchor(gridPtr?gridPtr->anchor:GRID_DEFAULT_ANCHOR),
		-1));
	return TCL_OK;
    }

    InitContainerData(containerPtr);
    gridPtr = containerPtr->containerDataPtr;
    old = gridPtr->anchor;
    if (Tk_GetAnchorFromObj(interp, objv[3], &gridPtr->anchor) != TCL_OK) {
	return TCL_ERROR;
    }

    /*
     * Only request a relayout if the anchor changes.
     */

    if (old != gridPtr->anchor) {
	if (containerPtr->abortPtr != NULL) {
	    *containerPtr->abortPtr = 1;
	}
	if (!(containerPtr->flags & REQUESTED_RELAYOUT)) {
	    containerPtr->flags |= REQUESTED_RELAYOUT;
	    Tcl_DoWhenIdle(ArrangeGrid, containerPtr);
	}
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * GridBboxCommand --
 *
 *	Implementation of the [grid bbox] subcommand.
 *
 * Results:
 *	Standard Tcl result.
 *
 * Side effects:
 *	Places bounding box information in the interp's result field.
 *
 *----------------------------------------------------------------------
 */

static int
GridBboxCommand(
    Tk_Window tkwin,		/* Main window of the application. */
    Tcl_Interp *interp,		/* Current interpreter. */
    Tcl_Size objc,			/* Number of arguments. */
    Tcl_Obj *const objv[])	/* Argument objects. */
{
    Tk_Window container;
    Gridder *containerPtr;		/* container grid record */
    GridContainer *gridPtr;	/* pointer to grid data */
    int row, column;		/* origin for bounding box */
    int row2, column2;		/* end of bounding box */
    int endX, endY;		/* last column/row in the layout */
    int x=0, y=0;		/* starting pixels for this bounding box */
    int width, height;		/* size of the bounding box */

    if (objc!=3 && objc != 5 && objc != 7) {
	Tcl_WrongNumArgs(interp, 2, objv, "window ?column row ?column row??");
	return TCL_ERROR;
    }

    if (TkGetWindowFromObj(interp, tkwin, objv[2], &container) != TCL_OK) {
	return TCL_ERROR;
    }
    if (!(containerPtr = GetGrid(container))) {
	return TCL_OK;
    }

    if (objc >= 5) {
	if (Tcl_GetIntFromObj(interp, objv[3], &column) != TCL_OK) {
	    return TCL_ERROR;
	}
	if (Tcl_GetIntFromObj(interp, objv[4], &row) != TCL_OK) {
	    return TCL_ERROR;
	}
	column2 = column;
	row2 = row;
    }

    if (objc == 7) {
	if (Tcl_GetIntFromObj(interp, objv[5], &column2) != TCL_OK) {
	    return TCL_ERROR;
	}
	if (Tcl_GetIntFromObj(interp, objv[6], &row2) != TCL_OK) {
	    return TCL_ERROR;
	}
    }

    gridPtr = containerPtr->containerDataPtr;
    if (gridPtr == NULL) {
	Tcl_SetObjResult(interp, NewQuadObj(0, 0, 0, 0));
	return TCL_OK;
    }

    SetGridSize(containerPtr);
    endX = MAX(gridPtr->columnEnd, gridPtr->columnMax);
    endY = MAX(gridPtr->rowEnd, gridPtr->rowMax);

    if ((endX == 0) || (endY == 0)) {
	Tcl_SetObjResult(interp, NewQuadObj(0, 0, 0, 0));
	return TCL_OK;
    }
    if (objc == 3) {
	row = 0;
	column = 0;
	row2 = endY;
	column2 = endX;
    }

    if (column > column2) {
	int temp = column;

	column = column2;
	column2 = temp;
    }
    if (row > row2) {
	int temp = row;

	row = row2;
	row2 = temp;
    }

    if (column > 0 && column < endX) {
	x = gridPtr->columnPtr[column-1].offset;
    } else if (column > 0) {
	x = gridPtr->columnPtr[endX-1].offset;
    }

    if (row > 0 && row < endY) {
	y = gridPtr->rowPtr[row-1].offset;
    } else if (row > 0) {
	y = gridPtr->rowPtr[endY-1].offset;
    }

    if (column2 < 0) {
	width = 0;
    } else if (column2 >= endX) {
	width = gridPtr->columnPtr[endX-1].offset - x;
    } else {
	width = gridPtr->columnPtr[column2].offset - x;
    }

    if (row2 < 0) {
	height = 0;
    } else if (row2 >= endY) {
	height = gridPtr->rowPtr[endY-1].offset - y;
    } else {
	height = gridPtr->rowPtr[row2].offset - y;
    }

    Tcl_SetObjResult(interp, NewQuadObj(
	    x + gridPtr->startX, y + gridPtr->startY, width, height));
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * GridForgetRemoveCommand --
 *
 *	Implementation of the [grid forget]/[grid remove] subcommands. See the
 *	user documentation for details on what these do.
 *
 * Results:
 *	Standard Tcl result.
 *
 * Side effects:
 *	Removes a window from a grid layout.
 *
 *----------------------------------------------------------------------
 */

static int
GridForgetRemoveCommand(
    Tk_Window tkwin,		/* Main window of the application. */
    Tcl_Interp *interp,		/* Current interpreter. */
    Tcl_Size objc,			/* Number of arguments. */
    Tcl_Obj *const objv[])	/* Argument objects. */
{
    Tk_Window content;
    Gridder *contentPtr;
    Tcl_Size i;
    const char *string = Tcl_GetString(objv[1]);
    char c = string[0];

    for (i = 2; i < objc; i++) {
	if (TkGetWindowFromObj(interp, tkwin, objv[i], &content) != TCL_OK) {
	    return TCL_ERROR;
	}

	if (!(contentPtr = GetGrid(content))) {
	    continue;
	}
	if (contentPtr->containerPtr != NULL) {
	    /*
	     * For "forget", reset all the settings to their defaults
	     */

	    if (c == 'f') {
		contentPtr->column = -1;
		contentPtr->row = -1;
		contentPtr->numCols = 1;
		contentPtr->numRows = 1;
		contentPtr->padX = 0;
		contentPtr->padY = 0;
		contentPtr->padLeft = 0;
		contentPtr->padTop = 0;
		contentPtr->iPadX = 0;
		contentPtr->iPadY = 0;
		if (contentPtr->in != NULL) {
		    Tcl_DecrRefCount(contentPtr->in);
		    contentPtr->in = NULL;
		}
		contentPtr->doubleBw = 2*Tk_Changes(tkwin)->border_width;
		if (contentPtr->flags & REQUESTED_RELAYOUT) {
		    Tcl_CancelIdleCall(ArrangeGrid, contentPtr);
		}
		contentPtr->flags = 0;
		contentPtr->sticky = 0;
	    } else {
		/*
		 * When removing, store name of container to be able to
		 * restore it later, even if the container is recreated.
		 */

		if (contentPtr->in != NULL) {
		    Tcl_DecrRefCount(contentPtr->in);
		    contentPtr->in = NULL;
		}
		if (contentPtr->containerPtr != NULL) {
		    contentPtr->in = Tcl_NewStringObj(
			    Tk_PathName(contentPtr->containerPtr->tkwin), TCL_INDEX_NONE);
		    Tcl_IncrRefCount(contentPtr->in);
		}
	    }
	    Tk_ManageGeometry(content, NULL, NULL);
	    if (contentPtr->containerPtr->tkwin != Tk_Parent(contentPtr->tkwin)) {
		Tk_UnmaintainGeometry(contentPtr->tkwin,
			contentPtr->containerPtr->tkwin);
	    }
	    Unlink(contentPtr);
	    Tk_UnmapWindow(contentPtr->tkwin);
	}
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * GridInfoCommand --
 *
 *	Implementation of the [grid info] subcommand. See the user
 *	documentation for details on what it does.
 *
 * Results:
 *	Standard Tcl result.
 *
 * Side effects:
 *	Puts gridding information in the interpreter's result.
 *
 *----------------------------------------------------------------------
 */

static int
GridInfoCommand(
    Tk_Window tkwin,		/* Main window of the application. */
    Tcl_Interp *interp,		/* Current interpreter. */
    Tcl_Size objc,			/* Number of arguments. */
    Tcl_Obj *const objv[])	/* Argument objects. */
{
    Gridder *contentPtr;
    Tk_Window content;
    Tcl_Obj *infoObj;

    if (objc != 3) {
	Tcl_WrongNumArgs(interp, 2, objv, "window");
	return TCL_ERROR;
    }
    if (TkGetWindowFromObj(interp, tkwin, objv[2], &content) != TCL_OK) {
	return TCL_ERROR;
    }
    if (!(contentPtr = GetGrid(content))) {
	return TCL_OK;
    }
    if (contentPtr->containerPtr == NULL) {
	Tcl_ResetResult(interp);
	return TCL_OK;
    }

    infoObj = Tcl_NewObj();
    Tcl_DictObjPut(NULL, infoObj, Tcl_NewStringObj("-in", TCL_INDEX_NONE),
	    Tk_NewWindowObj(contentPtr->containerPtr->tkwin));
    Tcl_DictObjPut(NULL, infoObj, Tcl_NewStringObj("-column", TCL_INDEX_NONE),
	    Tcl_NewWideIntObj(contentPtr->column));
    Tcl_DictObjPut(NULL, infoObj, Tcl_NewStringObj("-row", TCL_INDEX_NONE),
	    Tcl_NewWideIntObj(contentPtr->row));
    Tcl_DictObjPut(NULL, infoObj, Tcl_NewStringObj("-columnspan", TCL_INDEX_NONE),
	    Tcl_NewWideIntObj(contentPtr->numCols));
    Tcl_DictObjPut(NULL, infoObj, Tcl_NewStringObj("-rowspan", TCL_INDEX_NONE),
	    Tcl_NewWideIntObj(contentPtr->numRows));
    TkAppendPadAmount(infoObj, "-ipadx", contentPtr->iPadX/2, contentPtr->iPadX);
    TkAppendPadAmount(infoObj, "-ipady", contentPtr->iPadY/2, contentPtr->iPadY);
    TkAppendPadAmount(infoObj, "-padx", contentPtr->padLeft, contentPtr->padX);
    TkAppendPadAmount(infoObj, "-pady", contentPtr->padTop, contentPtr->padY);
    Tcl_DictObjPut(NULL, infoObj, Tcl_NewStringObj("-sticky", TCL_INDEX_NONE),
	    StickyToObj(contentPtr->sticky));
    Tcl_SetObjResult(interp, infoObj);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * GridLocationCommand --
 *
 *	Implementation of the [grid location] subcommand. See the user
 *	documentation for details on what it does.
 *
 * Results:
 *	Standard Tcl result.
 *
 * Side effects:
 *	Puts location information in the interpreter's result field.
 *
 *----------------------------------------------------------------------
 */

static int
GridLocationCommand(
    Tk_Window tkwin,		/* Main window of the application. */
    Tcl_Interp *interp,		/* Current interpreter. */
    Tcl_Size objc,			/* Number of arguments. */
    Tcl_Obj *const objv[])	/* Argument objects. */
{
    Tk_Window container;
    Gridder *containerPtr;		/* Container grid record. */
    GridContainer *gridPtr;	/* Pointer to grid data. */
    SlotInfo *slotPtr;
    int x, y;			/* Offset in pixels, from edge of container. */
    int i, j;			/* Corresponding column and row indeces. */
    int endX, endY;		/* End of grid. */

    if (objc != 5) {
	Tcl_WrongNumArgs(interp, 2, objv, "window x y");
	return TCL_ERROR;
    }

    if (TkGetWindowFromObj(interp, tkwin, objv[2], &container) != TCL_OK) {
	return TCL_ERROR;
    }

    if (Tk_GetPixelsFromObj(interp, container, objv[3], &x) != TCL_OK) {
	return TCL_ERROR;
    }
    if (Tk_GetPixelsFromObj(interp, container, objv[4], &y) != TCL_OK) {
	return TCL_ERROR;
    }

    if (!(containerPtr = GetGrid(container))) {
	return TCL_OK;
    }
    if (containerPtr->containerDataPtr == NULL) {
	Tcl_SetObjResult(interp, NewPairObj(-1, TCL_INDEX_NONE));
	return TCL_OK;
    }
    gridPtr = containerPtr->containerDataPtr;

    /*
     * Update any pending requests. This is not always the steady state value,
     * as more configure events could be in the pipeline, but its as close as
     * its easy to get.
     */

    while (containerPtr->flags & REQUESTED_RELAYOUT) {
	Tcl_CancelIdleCall(ArrangeGrid, containerPtr);
	ArrangeGrid(containerPtr);
    }
    SetGridSize(containerPtr);
    endX = MAX(gridPtr->columnEnd, gridPtr->columnMax);
    endY = MAX(gridPtr->rowEnd, gridPtr->rowMax);

    slotPtr = containerPtr->containerDataPtr->columnPtr;
    if (x < containerPtr->containerDataPtr->startX) {
	i = -1;
    } else {
	x -= containerPtr->containerDataPtr->startX;
	for (i = 0; slotPtr[i].offset < x && i < endX; i++) {
	    /* null body */
	}
    }

    slotPtr = containerPtr->containerDataPtr->rowPtr;
    if (y < containerPtr->containerDataPtr->startY) {
	j = -1;
    } else {
	y -= containerPtr->containerDataPtr->startY;
	for (j = 0; slotPtr[j].offset < y && j < endY; j++) {
	    /* null body */
	}
    }

    Tcl_SetObjResult(interp, NewPairObj(i, j));
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * GridPropagateCommand --
 *
 *	Implementation of the [grid propagate] subcommand. See the user
 *	documentation for details on what it does.
 *
 * Results:
 *	Standard Tcl result.
 *
 * Side effects:
 *	May alter geometry propagation for a widget.
 *
 *----------------------------------------------------------------------
 */

static int
GridPropagateCommand(
    Tk_Window tkwin,		/* Main window of the application. */
    Tcl_Interp *interp,		/* Current interpreter. */
    Tcl_Size objc,			/* Number of arguments. */
    Tcl_Obj *const objv[])	/* Argument objects. */
{
    Tk_Window container;
    Gridder *containerPtr;
    int propagate, old;

    if (objc > 4) {
	Tcl_WrongNumArgs(interp, 2, objv, "window ?boolean?");
	return TCL_ERROR;
    }

    if (TkGetWindowFromObj(interp, tkwin, objv[2], &container) != TCL_OK) {
	return TCL_ERROR;
    }
    if (!(containerPtr = GetGrid(container))) {
	return TCL_OK;
    }
    if (objc == 3) {
	Tcl_SetObjResult(interp,
		Tcl_NewBooleanObj(!(containerPtr->flags & DONT_PROPAGATE)));
	return TCL_OK;
    }
    if (Tcl_GetBooleanFromObj(interp, objv[3], &propagate) != TCL_OK) {
	return TCL_ERROR;
    }

    /*
     * Only request a relayout if the propagation bit changes.
     */

    old = !(containerPtr->flags & DONT_PROPAGATE);
    if (propagate != old) {
	if (propagate) {
	    /*
	     * If we have content, we need to register as geometry container.
	     */

	    if (containerPtr->contentPtr != NULL) {
		if (TkSetGeometryContainer(interp, container, "grid")	!= TCL_OK) {
		    return TCL_ERROR;
		}
		containerPtr->flags |= ALLOCED_CONTAINER;
	    }
	    containerPtr->flags &= ~DONT_PROPAGATE;
	} else {
	    if (containerPtr->flags & ALLOCED_CONTAINER) {
		TkFreeGeometryContainer(container, "grid");
		containerPtr->flags &= ~ALLOCED_CONTAINER;
	    }
	    containerPtr->flags |= DONT_PROPAGATE;
	}

	/*
	 * Re-arrange the container to allow new geometry information to
	 * propagate upwards to the container's container.
	 */

	if (containerPtr->abortPtr != NULL) {
	    *containerPtr->abortPtr = 1;
	}
	if (!(containerPtr->flags & REQUESTED_RELAYOUT)) {
	    containerPtr->flags |= REQUESTED_RELAYOUT;
	    Tcl_DoWhenIdle(ArrangeGrid, containerPtr);
	}
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * GridRowColumnConfigureCommand --
 *
 *	Implementation of the [grid rowconfigure] and [grid columnconfigure]
 *	subcommands. See the user documentation for details on what these do.
 *
 * Results:
 *	Standard Tcl result.
 *
 * Side effects:
 *	Depends on arguments; see user documentation.
 *
 *----------------------------------------------------------------------
 */

static int
GridRowColumnConfigureCommand(
    Tk_Window tkwin,		/* Main window of the application. */
    Tcl_Interp *interp,		/* Current interpreter. */
    Tcl_Size objc,			/* Number of arguments. */
    Tcl_Obj *const objv[])	/* Argument objects. */
{
    Tk_Window container, content;
    Gridder *containerPtr, *contentPtr;
    SlotInfo *slotPtr = NULL;
    int slot;			/* the column or row number */
    int slotType;		/* COLUMN or ROW */
    int size;			/* the configuration value */
    Tcl_Size lObjc;		/* Number of items in index list */
    Tcl_Obj **lObjv;		/* array of indices */
    int ok;			/* temporary TCL result code */
    Tcl_Size i, j, first, last;
    const char *string;
    static const char *const optionStrings[] = {
	"-minsize", "-pad", "-uniform", "-weight", NULL
    };
    enum options {
	ROWCOL_MINSIZE, ROWCOL_PAD, ROWCOL_UNIFORM, ROWCOL_WEIGHT
    };
    int index;
    Tcl_Obj *listCopy;

    if (((objc % 2 != 0) && (objc > 6)) || (objc < 4)) {
	Tcl_WrongNumArgs(interp, 2, objv, "window index ?-option value ...?");
	return TCL_ERROR;
    }

    if (TkGetWindowFromObj(interp, tkwin, objv[2], &container) != TCL_OK) {
	return TCL_ERROR;
    }

    listCopy = Tcl_DuplicateObj(objv[3]);
    Tcl_IncrRefCount(listCopy);
    if (Tcl_ListObjGetElements(interp, listCopy, &lObjc, &lObjv) != TCL_OK) {
	Tcl_DecrRefCount(listCopy);
	return TCL_ERROR;
    }

    string = Tcl_GetString(objv[1]);
    slotType = (*string == 'c') ? COLUMN : ROW;
    if (lObjc == 0) {
	Tcl_SetObjResult(interp, Tcl_ObjPrintf("no %s indices specified",
		(slotType == COLUMN) ? "column" : "row"));
	Tcl_SetErrorCode(interp, "TK", "GRID", "NO_INDEX", (char *)NULL);
	Tcl_DecrRefCount(listCopy);
	return TCL_ERROR;
    }

    if (!(containerPtr = GetGrid(container))) {
	return TCL_OK;
    }
    first = 0;
    last = 0;

    if ((objc == 4) || (objc == 5)) {
	if (lObjc != 1) {
	    Tcl_SetObjResult(interp, Tcl_NewStringObj(
		    "must specify a single element on retrieval", TCL_INDEX_NONE));
	    Tcl_SetErrorCode(interp, "TK", "GRID", "USAGE", (char *)NULL);
	    Tcl_DecrRefCount(listCopy);
	    return TCL_ERROR;
	}
	if (Tcl_GetIntFromObj(interp, lObjv[0], &slot) != TCL_OK) {
	    Tcl_AppendResult(interp,
		    " (when retrieving options only integer indices are "
		    "allowed)", NULL);
	    Tcl_SetErrorCode(interp, "TK", "GRID", "INDEX_FORMAT", (char *)NULL);
	    Tcl_DecrRefCount(listCopy);
	    return TCL_ERROR;
	}
	ok = CheckSlotData(containerPtr, slot, slotType, /* checkOnly */ 1);
	if (ok == TCL_OK) {
	    slotPtr = (slotType == COLUMN) ?
		    containerPtr->containerDataPtr->columnPtr :
		    containerPtr->containerDataPtr->rowPtr;
	}

	/*
	 * Return all of the options for this row or column. If the request is
	 * out of range, return all 0's.
	 */

	if (objc == 4) {
	    int minsize = 0, pad = 0, weight = 0;
	    Tk_Uid uniform = NULL;
	    Tcl_Obj *res = Tcl_NewListObj(0, NULL);

	    if (ok == TCL_OK) {
		minsize = slotPtr[slot].minSize;
		pad     = slotPtr[slot].pad;
		weight  = slotPtr[slot].weight;
		uniform = slotPtr[slot].uniform;
	    }

	    Tcl_ListObjAppendElement(interp, res,
		    Tcl_NewStringObj("-minsize", TCL_INDEX_NONE));
	    Tcl_ListObjAppendElement(interp, res, Tcl_NewWideIntObj(minsize));
	    Tcl_ListObjAppendElement(interp, res,
		    Tcl_NewStringObj("-pad", TCL_INDEX_NONE));
	    Tcl_ListObjAppendElement(interp, res, Tcl_NewWideIntObj(pad));
	    Tcl_ListObjAppendElement(interp, res,
		    Tcl_NewStringObj("-uniform", TCL_INDEX_NONE));
	    Tcl_ListObjAppendElement(interp, res,
		    Tcl_NewStringObj(uniform == NULL ? "" : uniform, TCL_INDEX_NONE));
	    Tcl_ListObjAppendElement(interp, res,
		    Tcl_NewStringObj("-weight", TCL_INDEX_NONE));
	    Tcl_ListObjAppendElement(interp, res, Tcl_NewWideIntObj(weight));
	    Tcl_SetObjResult(interp, res);
	    Tcl_DecrRefCount(listCopy);
	    return TCL_OK;
	}

	/*
	 * If only one option is given, with no value, the current value is
	 * returned.
	 */

	if (Tcl_GetIndexFromObjStruct(interp, objv[4], optionStrings,
		sizeof(char *), "option", 0, &index) != TCL_OK) {
	    Tcl_DecrRefCount(listCopy);
	    return TCL_ERROR;
	}
	if (index == ROWCOL_MINSIZE) {
	    Tcl_SetObjResult(interp, Tcl_NewWideIntObj(
		    (ok == TCL_OK) ? slotPtr[slot].minSize : 0));
	} else if (index == ROWCOL_WEIGHT) {
	    Tcl_SetObjResult(interp, Tcl_NewWideIntObj(
		    (ok == TCL_OK) ? slotPtr[slot].weight : 0));
	} else if (index == ROWCOL_UNIFORM) {
	    Tk_Uid value = (ok == TCL_OK) ? slotPtr[slot].uniform : "";

	    Tcl_SetObjResult(interp, Tcl_NewStringObj(
		    (value == NULL) ? "" : value, TCL_INDEX_NONE));
	} else if (index == ROWCOL_PAD) {
	    Tcl_SetObjResult(interp, Tcl_NewWideIntObj(
		    (ok == TCL_OK) ? slotPtr[slot].pad : 0));
	}
	Tcl_DecrRefCount(listCopy);
	return TCL_OK;
    }

    for (j = 0; j < lObjc; j++) {
	int allContent = 0;

	if (Tcl_GetIntFromObj(NULL, lObjv[j], &slot) == TCL_OK) {
	    first = slot;
	    last = slot;
	    contentPtr = NULL;
	} else if (strcmp(Tcl_GetString(lObjv[j]), "all") == 0) {
	    /*
	     * Make sure container is initialised.
	     */

	    InitContainerData(containerPtr);

	    contentPtr = containerPtr->contentPtr;
	    if (contentPtr == NULL) {
		continue;
	    }
	    allContent = 1;
	} else if (TkGetWindowFromObj(NULL, tkwin, lObjv[j], &content)
		== TCL_OK) {
	    /*
	     * Is it gridded in this container?
	     */

	    if (!(contentPtr = GetGrid(content))) {
		continue;
	    }
	    if (contentPtr->containerPtr != containerPtr) {
		Tcl_SetObjResult(interp, Tcl_ObjPrintf(
			"the window \"%s\" is not managed by \"%s\"",
			Tcl_GetString(lObjv[j]), Tcl_GetString(objv[2])));
		Tcl_SetErrorCode(interp, "TK", "GRID", "NOT_MANAGED", (char *)NULL);
		Tcl_DecrRefCount(listCopy);
		return TCL_ERROR;
	    }
	} else {
	    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
		    "illegal index \"%s\"", Tcl_GetString(lObjv[j])));
	    Tcl_SetErrorCode(interp, "TK", "VALUE", "GRID_INDEX", (char *)NULL);
	    Tcl_DecrRefCount(listCopy);
	    return TCL_ERROR;
	}

	/*
	 * The outer loop is only to handle "all".
	 */

	do {
	    if (contentPtr != NULL) {
		first = (slotType == COLUMN) ?
			contentPtr->column : contentPtr->row;
		last = first - 1 + ((slotType == COLUMN) ?
			contentPtr->numCols : contentPtr->numRows);
	    }

	    for (slot = first; slot <= last; slot++) {
		ok = CheckSlotData(containerPtr, slot, slotType, /*checkOnly*/ 0);
		if (ok != TCL_OK) {
		    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
			    "\"%s\" is out of range",
			    Tcl_GetString(lObjv[j])));
		    Tcl_SetErrorCode(interp, "TK", "GRID", "INDEX_RANGE",
			    (char *)NULL);
		    Tcl_DecrRefCount(listCopy);
		    return TCL_ERROR;
		}
		slotPtr = (slotType == COLUMN) ?
			containerPtr->containerDataPtr->columnPtr :
			containerPtr->containerDataPtr->rowPtr;

		/*
		 * Loop through each option value pair, setting the values as
		 * required.
		 */

		for (i = 4; i < objc; i += 2) {
		    if (Tcl_GetIndexFromObjStruct(interp, objv[i], optionStrings,
			    sizeof(char *), "option", 0, &index) != TCL_OK) {
			Tcl_DecrRefCount(listCopy);
			return TCL_ERROR;
		    }
		    if (index == ROWCOL_MINSIZE) {
			if (Tk_GetPixelsFromObj(interp, container, objv[i+1],
				&size) != TCL_OK) {
			    Tcl_DecrRefCount(listCopy);
			    return TCL_ERROR;
			} else {
			    slotPtr[slot].minSize = size;
			}
		    } else if (index == ROWCOL_WEIGHT) {
			int wt;

			if (Tcl_GetIntFromObj(interp,objv[i+1],&wt)!=TCL_OK) {
			    Tcl_DecrRefCount(listCopy);
			    return TCL_ERROR;
			} else if (wt < 0) {
			    Tcl_DecrRefCount(listCopy);
			    goto negativeIndex;
			} else {
			    slotPtr[slot].weight = wt;
			}
		    } else if (index == ROWCOL_UNIFORM) {
			slotPtr[slot].uniform =
				Tk_GetUid(Tcl_GetString(objv[i+1]));
			if (slotPtr[slot].uniform != NULL &&
				slotPtr[slot].uniform[0] == 0) {
			    slotPtr[slot].uniform = NULL;
			}
		    } else if (index == ROWCOL_PAD) {
			if (Tk_GetPixelsFromObj(interp, container, objv[i+1],
				&size) != TCL_OK) {
			    Tcl_DecrRefCount(listCopy);
			    return TCL_ERROR;
			} else if (size < 0) {
			    Tcl_DecrRefCount(listCopy);
			    goto negativeIndex;
			} else {
			    slotPtr[slot].pad = size;
			}
		    }
		}
	    }
	    if (contentPtr != NULL) {
		contentPtr = contentPtr->nextPtr;
	    }
	} while ((allContent == 1) && (contentPtr != NULL));
    }
    Tcl_DecrRefCount(listCopy);

    /*
     * We changed a property, re-arrange the table, and check for constraint
     * shrinkage. A null slotPtr will occur for 'all' checks.
     */

    if (slotPtr != NULL) {
	if (slotType == ROW) {
	    last = containerPtr->containerDataPtr->rowMax - 1;

	    while ((last >= 0) && (slotPtr[last].weight == 0)
		    && (slotPtr[last].pad == 0) && (slotPtr[last].minSize == 0)
		    && (slotPtr[last].uniform == NULL)) {
		last--;
	    }
	    containerPtr->containerDataPtr->rowMax = last+1;
	} else {
	    last = containerPtr->containerDataPtr->columnMax - 1;

	    while ((last >= 0) && (slotPtr[last].weight == 0)
		    && (slotPtr[last].pad == 0) && (slotPtr[last].minSize == 0)
		    && (slotPtr[last].uniform == NULL)) {
		last--;
	    }
	    containerPtr->containerDataPtr->columnMax = last + 1;
	}
    }

    if (containerPtr->abortPtr != NULL) {
	*containerPtr->abortPtr = 1;
    }
    if (!(containerPtr->flags & REQUESTED_RELAYOUT)) {
	containerPtr->flags |= REQUESTED_RELAYOUT;
	Tcl_DoWhenIdle(ArrangeGrid, containerPtr);
    }
    return TCL_OK;

  negativeIndex:
    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
	    "invalid arg \"%s\": should be non-negative",
	    Tcl_GetString(objv[i])));
    Tcl_SetErrorCode(interp, "TK", "GRID", "NEG_INDEX", (char *)NULL);
    return TCL_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * GridSizeCommand --
 *
 *	Implementation of the [grid size] subcommand. See the user
 *	documentation for details on what it does.
 *
 * Results:
 *	Standard Tcl result.
 *
 * Side effects:
 *	Puts grid size information in the interpreter's result.
 *
 *----------------------------------------------------------------------
 */

static int
GridSizeCommand(
    Tk_Window tkwin,		/* Main window of the application. */
    Tcl_Interp *interp,		/* Current interpreter. */
    Tcl_Size objc,			/* Number of arguments. */
    Tcl_Obj *const objv[])	/* Argument objects. */
{
    Tk_Window container;
    Gridder *containerPtr;
    GridContainer *gridPtr;	/* pointer to grid data */

    if (objc != 3) {
	Tcl_WrongNumArgs(interp, 2, objv, "window");
	return TCL_ERROR;
    }

    if (TkGetWindowFromObj(interp, tkwin, objv[2], &container) != TCL_OK) {
	return TCL_ERROR;
    }
    if (!(containerPtr = GetGrid(container))) {
	return TCL_OK;
    }

    if (containerPtr->containerDataPtr != NULL) {
	SetGridSize(containerPtr);
	gridPtr = containerPtr->containerDataPtr;
	Tcl_SetObjResult(interp, NewPairObj(
		MAX(gridPtr->columnEnd, gridPtr->columnMax),
		MAX(gridPtr->rowEnd, gridPtr->rowMax)));
    } else {
	Tcl_SetObjResult(interp, NewPairObj(0, 0));
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * GridContentCommand --
 *
 *	Implementation of the [grid content] subcommand. See the user
 *	documentation for details on what it does.
 *
 * Results:
 *	Standard Tcl result.
 *
 * Side effects:
 *	Places a list of content windows of the specified window in the
 *	interpreter's result field.
 *
 *----------------------------------------------------------------------
 */

static int
GridContentCommand(
    Tk_Window tkwin,		/* Main window of the application. */
    Tcl_Interp *interp,		/* Current interpreter. */
    Tcl_Size objc,			/* Number of arguments. */
    Tcl_Obj *const objv[])	/* Argument objects. */
{
    Tk_Window container;
    Gridder *containerPtr;		/* container grid record */
    Gridder *contentPtr;
    int i, value, index;
    int row = -1, column = -1;
    static const char *const optionStrings[] = {
	"-column", "-row", NULL
    };
    enum options { CONTENT_COLUMN, CONTENT_ROW };
    Tcl_Obj *res;

    if ((objc < 3) || ((objc % 2) == 0)) {
	Tcl_WrongNumArgs(interp, 2, objv, "window ?-option value ...?");
	return TCL_ERROR;
    }

    for (i = 3; i < objc; i += 2) {
	if (Tcl_GetIndexFromObjStruct(interp, objv[i], optionStrings,
		sizeof(char *), "option", 0, &index) != TCL_OK) {
	    return TCL_ERROR;
	}
	if (Tcl_GetIntFromObj(interp, objv[i+1], &value) != TCL_OK) {
	    return TCL_ERROR;
	}
	if (value < 0) {
	    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
		    "%d is an invalid value: should NOT be < 0", value));
	    Tcl_SetErrorCode(interp, "TK", "GRID", "NEG_INDEX", (char *)NULL);
	    return TCL_ERROR;
	}
	if (index == CONTENT_COLUMN) {
	    column = value;
	} else {
	    row = value;
	}
    }

    if (TkGetWindowFromObj(interp, tkwin, objv[2], &container) != TCL_OK) {
	return TCL_ERROR;
    }
    if (!(containerPtr = GetGrid(container))) {
	return TCL_OK;
    }

    res = Tcl_NewListObj(0, NULL);
    for (contentPtr = containerPtr->contentPtr; contentPtr != NULL;
	    contentPtr = contentPtr->nextPtr) {
	if ((column >= 0) && (contentPtr->column > column
		|| contentPtr->column+contentPtr->numCols-1 < column)) {
	    continue;
	}
	if ((row >= 0) && (contentPtr->row > row ||
		contentPtr->row+contentPtr->numRows-1 < row)) {
	    continue;
	}
	Tcl_ListObjAppendElement(interp,res, Tk_NewWindowObj(contentPtr->tkwin));
    }
    Tcl_SetObjResult(interp, res);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * GridReqProc --
 *
 *	This procedure is invoked by Tk_GeometryRequest for windows managed by
 *	the grid.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Arranges for tkwin, and all its managed siblings, to be re-arranged at
 *	the next idle point.
 *
 *----------------------------------------------------------------------
 */

static void
GridReqProc(
    void *clientData,	/* Grid's information about window that got
				 * new preferred geometry. */
    TCL_UNUSED(Tk_Window))		/* Other Tk-related information about the
				 * window. */
{
    Gridder *gridPtr = (Gridder *)clientData;

    gridPtr = gridPtr->containerPtr;
    if (gridPtr && !(gridPtr->flags & REQUESTED_RELAYOUT)) {
	gridPtr->flags |= REQUESTED_RELAYOUT;
	Tcl_DoWhenIdle(ArrangeGrid, gridPtr);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * GridLostContentProc --
 *
 *	This procedure is invoked by Tk whenever some other geometry claims
 *	control over a content that used to be managed by us.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Forgets all grid-related information about the content.
 *
 *----------------------------------------------------------------------
 */

static void
GridLostContentProc(
    void *clientData,	/* Grid structure for content window that was
				 * stolen away. */
    TCL_UNUSED(Tk_Window))		/* Tk's handle for the content window. */
{
    Gridder *contentPtr = (Gridder *)clientData;

    if (contentPtr->containerPtr->tkwin != Tk_Parent(contentPtr->tkwin)) {
	Tk_UnmaintainGeometry(contentPtr->tkwin, contentPtr->containerPtr->tkwin);
    }
    Unlink(contentPtr);
    Tk_UnmapWindow(contentPtr->tkwin);
}

/*
 *----------------------------------------------------------------------
 *
 * AdjustOffsets --
 *
 *	This procedure adjusts the size of the layout to fit in the space
 *	provided. If it needs more space, the extra is added according to the
 *	weights. If it needs less, the space is removed according to the
 *	weights, but at no time does the size drop below the minsize specified
 *	for that slot.
 *
 * Results:
 *	The size used by the layout.
 *
 * Side effects:
 *	The slot offsets are modified to shrink the layout.
 *
 *----------------------------------------------------------------------
 */

static int
AdjustOffsets(
    int size,			/* The total layout size (in pixels). */
    Tcl_Size slots,			/* Number of slots. */
    SlotInfo *slotPtr)	/* Pointer to slot array. */
{
    Tcl_Size slot;		/* Current slot. */
    int diff;			/* Extra pixels needed to add to the layout. */
    int totalWeight;		/* Sum of the weights for all the slots. */
    int weight;			/* Sum of the weights so far. */
    int minSize;		/* Minimum possible layout size. */
    int newDiff;		/* The most pixels that can be added on the
				 * current pass. */

    diff = size - slotPtr[slots-1].offset;

    /*
     * The layout is already the correct size; all done.
     */

    if (diff == 0) {
	return size;
    }

    /*
     * If all the weights are zero, there is nothing more to do.
     */

    totalWeight = 0;
    for (slot = 0; slot < slots; slot++) {
	totalWeight += slotPtr[slot].weight;
    }

    if (totalWeight == 0) {
	return slotPtr[slots-1].offset;
    }

    /*
     * Add extra space according to the slot weights. This is done
     * cumulatively to prevent round-off error accumulation.
     */

    if (diff > 0) {
	weight = 0;
	for (slot = 0; slot < slots; slot++) {
	    weight += slotPtr[slot].weight;
	    slotPtr[slot].offset += diff * weight / totalWeight;
	}
	return size;
    }

    /*
     * The layout must shrink below its requested size. Compute the minimum
     * possible size by looking at the slot minSizes. Store each slot's
     * minimum size in temp.
     */

    minSize = 0;
    for (slot = 0; slot < slots; slot++) {
	if (slotPtr[slot].weight > 0) {
	    slotPtr[slot].temp = slotPtr[slot].minSize;
	} else if (slot > 0) {
	    slotPtr[slot].temp = slotPtr[slot].offset - slotPtr[slot-1].offset;
	} else {
	    slotPtr[slot].temp = slotPtr[slot].offset;
	}
	minSize += slotPtr[slot].temp;
    }

    /*
     * If the requested size is less than the minimum required size, set the
     * slot sizes to their minimum values.
     */

    if (size <= minSize) {
	int offset = 0;

	for (slot = 0; slot < slots; slot++) {
	    offset += slotPtr[slot].temp;
	    slotPtr[slot].offset = offset;
	}
	return minSize;
    }

    /*
     * Remove space from slots according to their weights. The weights get
     * renormalized anytime a slot shrinks to its minimum size.
     */

    while (diff < 0) {
	/*
	 * Find the total weight for the shrinkable slots.
	 */

	totalWeight = 0;
	for (slot = 0; slot < slots; slot++) {
	    int current = (slot == 0) ? slotPtr[slot].offset :
		    slotPtr[slot].offset - slotPtr[slot-1].offset;

	    if (current > slotPtr[slot].minSize) {
		totalWeight += slotPtr[slot].weight;
		slotPtr[slot].temp = slotPtr[slot].weight;
	    } else {
		slotPtr[slot].temp = 0;
	    }
	}
	if (totalWeight == 0) {
	    break;
	}

	/*
	 * Find the maximum amount of space we can distribute this pass.
	 */

	newDiff = diff;
	for (slot = 0; slot < slots; slot++) {
	    int current;	/* Current size of this slot. */
	    int maxDiff;	/* Maximum diff that would cause this slot to
				 * equal its minsize. */

	    if (slotPtr[slot].temp == 0) {
		continue;
	    }
	    current = (slot == 0) ? slotPtr[slot].offset :
		    slotPtr[slot].offset - slotPtr[slot-1].offset;
	    maxDiff = totalWeight * (slotPtr[slot].minSize - current)
		    / slotPtr[slot].temp;
	    if (maxDiff > newDiff) {
		newDiff = maxDiff;
	    }
	}

	/*
	 * Now distribute the space.
	 */

	weight = 0;
	for (slot = 0; slot < slots; slot++) {
	    weight += slotPtr[slot].temp;
	    slotPtr[slot].offset += newDiff * weight / totalWeight;
	}
	diff -= newDiff;
    }
    return size;
}

/*
 *----------------------------------------------------------------------
 *
 * AdjustForSticky --
 *
 *	This procedure adjusts the size of a content in its cavity based on its
 *	"sticky" flags.
 *
 * Results:
 *	The input x, y, width, and height are changed to represent the desired
 *	coordinates of the content.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static void
AdjustForSticky(
    Gridder *contentPtr,	/* Content window to arrange in its cavity. */
    int *xPtr,		/* Pixel location of the left edge of the cavity. */
    int *yPtr,		/* Pixel location of the top edge of the cavity. */
    int *widthPtr,	/* Width of the cavity (in pixels). */
    int *heightPtr)	/* Height of the cavity (in pixels). */
{
    int diffx = 0;	/* Cavity width - content width. */
    int diffy = 0;	/* Cavity hight - content height. */
    int sticky = contentPtr->sticky;

    *xPtr += contentPtr->padLeft;
    *widthPtr -= contentPtr->padX;
    *yPtr += contentPtr->padTop;
    *heightPtr -= contentPtr->padY;

    if (*widthPtr > (Tk_ReqWidth(contentPtr->tkwin) + contentPtr->iPadX)) {
	diffx = *widthPtr - (Tk_ReqWidth(contentPtr->tkwin) + contentPtr->iPadX);
	*widthPtr = Tk_ReqWidth(contentPtr->tkwin) + contentPtr->iPadX;
    }

    if (*heightPtr > (Tk_ReqHeight(contentPtr->tkwin) + contentPtr->iPadY)) {
	diffy = *heightPtr - (Tk_ReqHeight(contentPtr->tkwin) + contentPtr->iPadY);
	*heightPtr = Tk_ReqHeight(contentPtr->tkwin) + contentPtr->iPadY;
    }

    if (sticky&STICK_EAST && sticky&STICK_WEST) {
	*widthPtr += diffx;
    }
    if (sticky&STICK_NORTH && sticky&STICK_SOUTH) {
	*heightPtr += diffy;
    }
    if (!(sticky&STICK_WEST)) {
	*xPtr += (sticky&STICK_EAST) ? diffx : diffx/2;
    }
    if (!(sticky&STICK_NORTH)) {
	*yPtr += (sticky&STICK_SOUTH) ? diffy : diffy/2;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * ArrangeGrid --
 *
 *	This procedure is invoked (using the Tcl_DoWhenIdle mechanism) to
 *	re-layout a set of windows managed by the grid. It is invoked at idle
 *	time so that a series of grid requests can be merged into a single
 *	layout operation.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The content of containerPtr may get resized or moved.
 *
 *----------------------------------------------------------------------
 */

static void
ArrangeGrid(
    void *clientData)	/* Structure describing container whose content
				 * are to be re-layed out. */
{
    Gridder *containerPtr = (Gridder *)clientData;
    Gridder *contentPtr;
    GridContainer *slotPtr = containerPtr->containerDataPtr;
    int abort;
    int width, height;		/* Requested size of layout, in pixels. */
    int realWidth, realHeight;	/* Actual size layout should take-up. */
    int usedX, usedY;

    containerPtr->flags &= ~REQUESTED_RELAYOUT;

    /*
     * If the container has no content anymore, then don't change the container size.
     * Otherwise there is no way to "relinquish" control over the container
     * so another geometry manager can take over.
     */

    if (containerPtr->contentPtr == NULL) {
	return;
    }

    if (containerPtr->containerDataPtr == NULL) {
	return;
    }

    /*
     * Abort any nested call to ArrangeGrid for this window, since we'll do
     * everything necessary here, and set up so this call can be aborted if
     * necessary.
     */

    if (containerPtr->abortPtr != NULL) {
	*containerPtr->abortPtr = 1;
    }
    containerPtr->abortPtr = &abort;
    abort = 0;
    Tcl_Preserve(containerPtr);

    /*
     * Call the constraint engine to fill in the row and column offsets.
     */

    SetGridSize(containerPtr);
    width = ResolveConstraints(containerPtr, COLUMN, 0);
    height = ResolveConstraints(containerPtr, ROW, 0);
    width += Tk_InternalBorderLeft(containerPtr->tkwin) +
	    Tk_InternalBorderRight(containerPtr->tkwin);
    height += Tk_InternalBorderTop(containerPtr->tkwin) +
	    Tk_InternalBorderBottom(containerPtr->tkwin);

    if (width < Tk_MinReqWidth(containerPtr->tkwin)) {
	width = Tk_MinReqWidth(containerPtr->tkwin);
    }
    if (height < Tk_MinReqHeight(containerPtr->tkwin)) {
	height = Tk_MinReqHeight(containerPtr->tkwin);
    }

    if (((width != Tk_ReqWidth(containerPtr->tkwin))
	    || (height != Tk_ReqHeight(containerPtr->tkwin)))
	    && !(containerPtr->flags & DONT_PROPAGATE)) {
	Tk_GeometryRequest(containerPtr->tkwin, width, height);
	if (width>1 && height>1) {
	    containerPtr->flags |= REQUESTED_RELAYOUT;
	    Tcl_DoWhenIdle(ArrangeGrid, containerPtr);
	}
	containerPtr->abortPtr = NULL;
	Tcl_Release(containerPtr);
	return;
    }

    /*
     * If the currently requested layout size doesn't match the container's
     * window size, then adjust the slot offsets according to the weights. If
     * all of the weights are zero, place the layout according to the anchor
     * value.
     */

    realWidth = Tk_Width(containerPtr->tkwin) -
	    Tk_InternalBorderLeft(containerPtr->tkwin) -
	    Tk_InternalBorderRight(containerPtr->tkwin);
    realHeight = Tk_Height(containerPtr->tkwin) -
	    Tk_InternalBorderTop(containerPtr->tkwin) -
	    Tk_InternalBorderBottom(containerPtr->tkwin);
    usedX = AdjustOffsets(realWidth,
	    MAX(slotPtr->columnEnd, slotPtr->columnMax), slotPtr->columnPtr);
    usedY = AdjustOffsets(realHeight, MAX(slotPtr->rowEnd, slotPtr->rowMax),
	    slotPtr->rowPtr);
    TkComputeAnchor(containerPtr->containerDataPtr->anchor, containerPtr->tkwin,
	    0, 0, usedX, usedY, &slotPtr->startX, &slotPtr->startY);

    /*
     * Now adjust the actual size of the content to its cavity by computing the
     * cavity size, and adjusting the widget according to its stickyness.
     */

    for (contentPtr = containerPtr->contentPtr; contentPtr != NULL && !abort;
	    contentPtr = contentPtr->nextPtr) {
	int x, y;			/* Top left coordinate */
	int col = contentPtr->column;
	int row = contentPtr->row;

	x = (col>0) ? slotPtr->columnPtr[col-1].offset : 0;
	y = (row>0) ? slotPtr->rowPtr[row-1].offset : 0;

	width = slotPtr->columnPtr[contentPtr->numCols+col-1].offset - x;
	height = slotPtr->rowPtr[contentPtr->numRows+row-1].offset - y;

	x += slotPtr->startX;
	y += slotPtr->startY;

	AdjustForSticky(contentPtr, &x, &y, &width, &height);

	/*
	 * Now put the window in the proper spot. (This was taken directly
	 * from tkPack.c.) If the content is a child of the container, then do this
	 * here. Otherwise let Tk_MaintainGeometry do the work.
	 */

	if (containerPtr->tkwin == Tk_Parent(contentPtr->tkwin)) {
	    if ((width <= 0) || (height <= 0)) {
		Tk_UnmapWindow(contentPtr->tkwin);
	    } else {
		if ((x != Tk_X(contentPtr->tkwin))
			|| (y != Tk_Y(contentPtr->tkwin))
			|| (width != Tk_Width(contentPtr->tkwin))
			|| (height != Tk_Height(contentPtr->tkwin))) {
		    Tk_MoveResizeWindow(contentPtr->tkwin, x, y, width, height);
		}
		if (abort) {
		    break;
		}

		/*
		 * Don't map the content if the container isn't mapped: wait until
		 * the container gets mapped later.
		 */

		if (Tk_IsMapped(containerPtr->tkwin)) {
		    Tk_MapWindow(contentPtr->tkwin);
		}
	    }
	} else if ((width <= 0) || (height <= 0)) {
	    Tk_UnmaintainGeometry(contentPtr->tkwin, containerPtr->tkwin);
	    Tk_UnmapWindow(contentPtr->tkwin);
	} else {
	    Tk_MaintainGeometry(contentPtr->tkwin, containerPtr->tkwin, x, y,
		    width, height);
	}
    }

    containerPtr->abortPtr = NULL;
    Tcl_Release(containerPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * ResolveConstraints --
 *
 *	Resolve all of the column and row boundaries. Most of the calculations
 *	are identical for rows and columns, so this procedure is called twice,
 *	once for rows, and again for columns.
 *
 * Results:
 *	The offset (in pixels) from the left/top edge of this layout is
 *	returned.
 *
 * Side effects:
 *	The slot offsets are copied into the SlotInfo structure for the
 *	geometry container.
 *
 *----------------------------------------------------------------------
 */

static int
ResolveConstraints(
    Gridder *containerPtr,		/* The geometry container for this grid. */
    int slotType,		/* Either ROW or COLUMN. */
    int maxOffset)		/* The actual maximum size of this layout in
				 * pixels, or 0 (not currently used). */
{
    SlotInfo *slotPtr;	/* Pointer to row/col constraints. */
    Gridder *contentPtr;	/* List of content windows in this grid. */
    int constraintCount;	/* Count of rows or columns that have
				 * constraints. */
    int slotCount;		/* Last occupied row or column. */
    int gridCount;		/* The larger of slotCount and
				 * constraintCount. */
    GridLayout *layoutPtr;	/* Temporary layout structure. */
    int requiredSize;		/* The natural size of the grid (pixels).
				 * This is the minimum size needed to
				 * accommodate all of the content at their
				 * requested sizes. */
    int offset;			/* The pixel offset of the right edge of the
				 * current slot from the beginning of the
				 * layout. */
    int slot;			/* The current slot. */
    int start;			/* The first slot of a contiguous set whose
				 * constraints are not yet fully resolved. */
    int end;			/* The Last slot of a contiguous set whose
				 * constraints are not yet fully resolved. */
    UniformGroup uniformPre[UNIFORM_PREALLOC];
				/* Pre-allocated space for uniform groups. */
    UniformGroup *uniformGroupPtr;
				/* Uniform groups data. */
    int uniformGroups;		/* Number of currently used uniform groups. */
    int uniformGroupsAlloced;	/* Size of allocated space for uniform
				 * groups. */
    int minSize;
    int prevGrow, accWeight, grow;

    /*
     * For typical sized tables, we'll use stack space for the layout data to
     * avoid the overhead of a malloc and free for every layout.
     */

    GridLayout layoutData[TYPICAL_SIZE + 1];

    if (slotType == COLUMN) {
	constraintCount = containerPtr->containerDataPtr->columnMax;
	slotCount = containerPtr->containerDataPtr->columnEnd;
	slotPtr = containerPtr->containerDataPtr->columnPtr;
    } else {
	constraintCount = containerPtr->containerDataPtr->rowMax;
	slotCount = containerPtr->containerDataPtr->rowEnd;
	slotPtr = containerPtr->containerDataPtr->rowPtr;
    }

    /*
     * Make sure there is enough memory for the layout.
     */

    gridCount = MAX(constraintCount, slotCount);
    if (gridCount >= TYPICAL_SIZE) {
	layoutPtr = (GridLayout *)ckalloc(sizeof(GridLayout) * (1+gridCount));
    } else {
	layoutPtr = layoutData;
    }

    /*
     * Allocate an extra layout slot to represent the left/top edge of the 0th
     * slot to make it easier to calculate slot widths from offsets without
     * special case code.
     *
     * Initialize the "dummy" slot to the left/top of the table. This slot
     * avoids special casing the first slot.
     */

    layoutPtr->minOffset = 0;
    layoutPtr->maxOffset = 0;
    layoutPtr++;

    /*
     * Step 1.
     * Copy the slot constraints into the layout structure, and initialize the
     * rest of the fields.
     */

    for (slot=0; slot < constraintCount; slot++) {
	layoutPtr[slot].minSize = slotPtr[slot].minSize;
	layoutPtr[slot].weight = slotPtr[slot].weight;
	layoutPtr[slot].uniform = slotPtr[slot].uniform;
	layoutPtr[slot].pad = slotPtr[slot].pad;
	layoutPtr[slot].binNextPtr = NULL;
    }
    for (; slot<gridCount; slot++) {
	layoutPtr[slot].minSize = 0;
	layoutPtr[slot].weight = 0;
	layoutPtr[slot].uniform = NULL;
	layoutPtr[slot].pad = 0;
	layoutPtr[slot].binNextPtr = NULL;
    }

    /*
     * Step 2.
     * Content with a span of 1 are used to determine the minimum size of each
     * slot. Content whose span is two or more slots don't contribute to the
     * minimum size of each slot directly, but can cause slots to grow if
     * their size exceeds the the sizes of the slots they span.
     *
     * Bin all content whose spans are > 1 by their right edges. This allows
     * the computation on minimum and maximum possible layout sizes at each
     * slot boundary, without the need to re-sort the content.
     */

    switch (slotType) {
    case COLUMN:
	for (contentPtr = containerPtr->contentPtr; contentPtr != NULL;
		contentPtr = contentPtr->nextPtr) {
	    int rightEdge = contentPtr->column + contentPtr->numCols - 1;

	    contentPtr->size = Tk_ReqWidth(contentPtr->tkwin) + contentPtr->padX
		    + contentPtr->iPadX + contentPtr->doubleBw;
	    if (contentPtr->numCols > 1) {
		contentPtr->binNextPtr = layoutPtr[rightEdge].binNextPtr;
		layoutPtr[rightEdge].binNextPtr = contentPtr;
	    } else if (rightEdge >= 0) {
		int size = contentPtr->size + layoutPtr[rightEdge].pad;

		if (size > layoutPtr[rightEdge].minSize) {
		    layoutPtr[rightEdge].minSize = size;
		}
	    }
	}
	break;
    case ROW:
	for (contentPtr = containerPtr->contentPtr; contentPtr != NULL;
		contentPtr = contentPtr->nextPtr) {
	    int rightEdge = contentPtr->row + contentPtr->numRows - 1;

	    contentPtr->size = Tk_ReqHeight(contentPtr->tkwin) + contentPtr->padY
		    + contentPtr->iPadY + contentPtr->doubleBw;
	    if (contentPtr->numRows > 1) {
		contentPtr->binNextPtr = layoutPtr[rightEdge].binNextPtr;
		layoutPtr[rightEdge].binNextPtr = contentPtr;
	    } else if (rightEdge >= 0) {
		int size = contentPtr->size + layoutPtr[rightEdge].pad;

		if (size > layoutPtr[rightEdge].minSize) {
		    layoutPtr[rightEdge].minSize = size;
		}
	    }
	}
	break;
    }

    /*
     * Step 2b.
     * Consider demands on uniform sizes.
     */

    uniformGroupPtr = uniformPre;
    uniformGroupsAlloced = UNIFORM_PREALLOC;
    uniformGroups = 0;

    for (slot = 0; slot < gridCount; slot++) {
	if (layoutPtr[slot].uniform != NULL) {
		int weight;
	    for (start = 0; start < uniformGroups; start++) {
		if (uniformGroupPtr[start].group == layoutPtr[slot].uniform) {
		    break;
		}
	    }
	    if (start >= uniformGroups) {
		/*
		 * Have not seen that group before, set up data for it.
		 */

		if (uniformGroups >= uniformGroupsAlloced) {
		    /*
		     * We need to allocate more space.
		     */

		    size_t oldSize = uniformGroupsAlloced
			    * sizeof(UniformGroup);
		    size_t newSize = (uniformGroupsAlloced + UNIFORM_PREALLOC)
			    * sizeof(UniformGroup);
		    UniformGroup *newUG = (UniformGroup *)ckalloc(newSize);
		    UniformGroup *oldUG = uniformGroupPtr;

		    memcpy(newUG, oldUG, oldSize);
		    if (oldUG != uniformPre) {
			ckfree(oldUG);
		    }
		    uniformGroupPtr = newUG;
		    uniformGroupsAlloced += UNIFORM_PREALLOC;
		}
		uniformGroups++;
		uniformGroupPtr[start].group = layoutPtr[slot].uniform;
		uniformGroupPtr[start].minSize = 0;
	    }
	    weight = layoutPtr[slot].weight;
	    weight = weight > 0 ? weight : 1;
	    minSize = (layoutPtr[slot].minSize + weight - 1) / weight;
	    if (minSize > uniformGroupPtr[start].minSize) {
		uniformGroupPtr[start].minSize = minSize;
	    }
	}
    }

    /*
     * Data has been gathered about uniform groups. Now relayout accordingly.
     */

    if (uniformGroups > 0) {
	for (slot = 0; slot < gridCount; slot++) {
	    if (layoutPtr[slot].uniform != NULL) {
		for (start = 0; start < uniformGroups; start++) {
		    if (uniformGroupPtr[start].group ==
			    layoutPtr[slot].uniform) {
			int weight = layoutPtr[slot].weight;
			weight = weight > 0 ? weight : 1;
			layoutPtr[slot].minSize =
				uniformGroupPtr[start].minSize * weight;
			break;
		    }
		}
	    }
	}
    }

    if (uniformGroupPtr != uniformPre) {
	ckfree(uniformGroupPtr);
    }

    /*
     * Step 3.
     * Determine the minimum slot offsets going from left to right that would
     * fit all of the content. This determines the minimum
     */

    for (offset=0,slot=0; slot < gridCount; slot++) {
	layoutPtr[slot].minOffset = layoutPtr[slot].minSize + offset;
	for (contentPtr = layoutPtr[slot].binNextPtr; contentPtr != NULL;
		contentPtr = contentPtr->binNextPtr) {
	    int span = (slotType == COLUMN) ?
		    contentPtr->numCols : contentPtr->numRows;
	    int required = contentPtr->size + layoutPtr[slot - span].minOffset;

	    if (required > layoutPtr[slot].minOffset) {
		layoutPtr[slot].minOffset = required;
	    }
	}
	offset = layoutPtr[slot].minOffset;
    }

    /*
     * At this point, we know the minimum required size of the entire layout.
     * It might be prudent to stop here if our "container" will resize itself to
     * this size.
     */

    requiredSize = offset;
    if (maxOffset > offset) {
	offset=maxOffset;
    }

    /*
     * Step 4.
     * Determine the minimum slot offsets going from right to left, bounding
     * the pixel range of each slot boundary. Pre-fill all of the right
     * offsets with the actual size of the table; they will be reduced as
     * required.
     */

    for (slot=0; slot < gridCount; slot++) {
	layoutPtr[slot].maxOffset = offset;
    }
    for (slot=gridCount-1; slot > 0;) {
	for (contentPtr = layoutPtr[slot].binNextPtr; contentPtr != NULL;
		contentPtr = contentPtr->binNextPtr) {
	    int span = (slotType == COLUMN) ?
		    contentPtr->numCols : contentPtr->numRows;
	    int require = offset - contentPtr->size;
	    int startSlot = slot - span;

	    if (startSlot >=0 && require < layoutPtr[startSlot].maxOffset) {
		layoutPtr[startSlot].maxOffset = require;
	    }
	}
	offset -= layoutPtr[slot].minSize;
	slot--;
	if (layoutPtr[slot].maxOffset < offset) {
	    offset = layoutPtr[slot].maxOffset;
	} else {
	    layoutPtr[slot].maxOffset = offset;
	}
    }

    /*
     * Step 5.
     * At this point, each slot boundary has a range of values that will
     * satisfy the overall layout size. Make repeated passes over the layout
     * structure looking for spans of slot boundaries where the minOffsets are
     * less than the maxOffsets, and adjust the offsets according to the slot
     * weights. At each pass, at least one slot boundary will have its range
     * of possible values fixed at a single value.
     */

    for (start = 0; start < gridCount;) {
	int totalWeight = 0;	/* Sum of the weights for all of the slots in
				 * this span. */
	int need = 0;		/* The minimum space needed to layout this
				 * span. */
	int have;		/* The actual amount of space that will be
				 * taken up by this span. */
	int weight;		/* Cumulative weights of the columns in this
				 * span. */
	int noWeights = 0;	/* True if the span has no weights. */

	/*
	 * Find a span by identifying ranges of slots whose edges are already
	 * constrained at fixed offsets, but whose internal slot boundaries
	 * have a range of possible positions.
	 */

	if (layoutPtr[start].minOffset == layoutPtr[start].maxOffset) {
	    start++;
	    continue;
	}

	for (end = start + 1; end < gridCount; end++) {
	    if (layoutPtr[end].minOffset == layoutPtr[end].maxOffset) {
		break;
	    }
	}

	/*
	 * We found a span. Compute the total weight, minumum space required,
	 * for this span, and the actual amount of space the span should use.
	 */

	for (slot = start; slot <= end; slot++) {
	    totalWeight += layoutPtr[slot].weight;
	    need += layoutPtr[slot].minSize;
	}
	have = layoutPtr[end].maxOffset - layoutPtr[start-1].minOffset;

	/*
	 * If all the weights in the span are zero, then distribute the extra
	 * space evenly.
	 */

	if (totalWeight == 0) {
	    noWeights++;
	    totalWeight = end - start + 1;
	}

	/*
	 * It might not be possible to give the span all of the space
	 * available on this pass without violating the size constraints of
	 * one or more of the internal slot boundaries. Try to determine the
	 * maximum amount of space that when added to the entire span, would
	 * cause a slot boundary to have its possible range reduced to one
	 * value, and reduce the amount of extra space allocated on this pass
	 * accordingly.
	 *
	 * The calculation is done cumulatively to avoid accumulating roundoff
	 * errors.
	 */

	do {
	    int prevMinOffset = layoutPtr[start - 1].minOffset;

	    prevGrow = 0;
	    accWeight = 0;
	    for (slot = start; slot <= end; slot++) {
		weight = noWeights ? 1 : layoutPtr[slot].weight;
		accWeight += weight;
		grow = (have - need) * accWeight / totalWeight - prevGrow;
		prevGrow += grow;

		if ((weight > 0) &&
			((prevMinOffset + layoutPtr[slot].minSize + grow)
			> layoutPtr[slot].maxOffset)) {
		    int newHave;

		    /*
		     * There is not enough room to grow that much. Calculate
		     * how much this slot can grow and how much "have" that
		     * corresponds to.
		     */

		    grow = layoutPtr[slot].maxOffset -
			    layoutPtr[slot].minSize - prevMinOffset;
		    newHave = grow * totalWeight / weight;
		    if (newHave > totalWeight) {
			/*
			 * By distributing multiples of totalWeight we
			 * minimize rounding errors since they will only
			 * happen in the last loop(s).
			 */

			newHave = newHave / totalWeight * totalWeight;
		    }
		    if (newHave <= 0) {
			/*
			 * We can end up with a "have" of 0 here if the
			 * previous slots have taken all the space. In that
			 * case we cannot guess an appropriate "have" so we
			 * just try some lower "have" that is >= 1, to make
			 * sure this terminates.
			 */

			newHave = (have - need) - 1;
			if (newHave > (3 * totalWeight)) {
			    /*
			     * Go down 25% for large values.
			     */
			    newHave = newHave * 3 / 4;
			}

			if (newHave > totalWeight) {
			    /*
			     * Round down to a multiple of totalWeight.
			     */
			    newHave = newHave / totalWeight * totalWeight;
			}

			if (newHave <= 0) {
			    newHave = 1;
			}
		    }
		    have = newHave + need;

		    /*
		     * Restart loop to check if the new "have" will fit.
		     */

		    break;
		}
		prevMinOffset += layoutPtr[slot].minSize + grow;
		if (prevMinOffset < layoutPtr[slot].minOffset) {
		    prevMinOffset = layoutPtr[slot].minOffset;
		}
	    }

	    /*
	     * Quit the outer loop if the inner loop ran all the way.
	     */
	} while (slot <= end);

	/*
	 * Now distribute the extra space among the slots by adjusting the
	 * minSizes and minOffsets.
	 */

	prevGrow = 0;
	accWeight = 0;
	for (slot = start; slot <= end; slot++) {
	    accWeight += noWeights ? 1 : layoutPtr[slot].weight;
	    grow = (have - need) * accWeight / totalWeight - prevGrow;
	    prevGrow += grow;
	    layoutPtr[slot].minSize += grow;
	    if ((layoutPtr[slot-1].minOffset + layoutPtr[slot].minSize)
		    > layoutPtr[slot].minOffset) {
		layoutPtr[slot].minOffset = layoutPtr[slot-1].minOffset +
			layoutPtr[slot].minSize;
	    }
	}

	/*
	 * Having pushed the top/left boundaries of the slots to take up extra
	 * space, the bottom/right space is recalculated to propagate the new
	 * space allocation.
	 */

	for (slot = end; slot > start; slot--) {
	    /*
	     * maxOffset may not go up.
	     */

	    if ((layoutPtr[slot].maxOffset-layoutPtr[slot].minSize)
		    < layoutPtr[slot-1].maxOffset) {
		layoutPtr[slot-1].maxOffset =
			layoutPtr[slot].maxOffset-layoutPtr[slot].minSize;
	    }
	}
    }

    /*
     * Step 6.
     * All of the space has been apportioned; copy the layout information back
     * into the container.
     */

    for (slot=0; slot < gridCount; slot++) {
	slotPtr[slot].offset = layoutPtr[slot].minOffset;
    }

    --layoutPtr;
    if (layoutPtr != layoutData) {
	ckfree(layoutPtr);
    }
    return requiredSize;
}

/*
 *----------------------------------------------------------------------
 *
 * GetGrid --
 *
 *	This internal procedure is used to locate a Grid structure for a given
 *	window, creating one if one doesn't exist already, except if the window
 *	is already dead.
 *
 * Results:
 *	The return value is a pointer to the Grid structure corresponding to
 *	tkwin, or NULL when tkwin is already dead.
 *
 * Side effects:
 *	A new grid structure may be created. If so, then a callback is set up
 *	to clean things up when the window is deleted.
 *
 *----------------------------------------------------------------------
 */

static Gridder *
GetGrid(
    Tk_Window tkwin)		/* Token for window for which grid structure
				 * is desired. */
{
    Gridder *gridPtr;
    Tcl_HashEntry *hPtr;
    int isNew;
    TkDisplay *dispPtr = ((TkWindow *) tkwin)->dispPtr;

    if (((TkWindow *) tkwin)->flags & TK_ALREADY_DEAD) {
	return NULL;
    }

    if (!dispPtr->gridInit) {
	Tcl_InitHashTable(&dispPtr->gridHashTable, TCL_ONE_WORD_KEYS);
	dispPtr->gridInit = 1;
    }

    /*
     * See if there's already grid for this window. If not, then create a new
     * one.
     */

    hPtr = Tcl_CreateHashEntry(&dispPtr->gridHashTable, (char*) tkwin, &isNew);
    if (!isNew) {
	return (Gridder *)Tcl_GetHashValue(hPtr);
    }
    gridPtr = (Gridder *)ckalloc(sizeof(Gridder));
    gridPtr->tkwin = tkwin;
    gridPtr->containerPtr = NULL;
    gridPtr->containerDataPtr = NULL;
    gridPtr->nextPtr = NULL;
    gridPtr->contentPtr = NULL;
    gridPtr->binNextPtr = NULL;

    gridPtr->column = -1;
    gridPtr->row = -1;
    gridPtr->numCols = 1;
    gridPtr->numRows = 1;

    gridPtr->padX = 0;
    gridPtr->padY = 0;
    gridPtr->padLeft = 0;
    gridPtr->padTop = 0;
    gridPtr->iPadX = 0;
    gridPtr->iPadY = 0;
    gridPtr->doubleBw = 2 * Tk_Changes(tkwin)->border_width;
    gridPtr->abortPtr = NULL;
    gridPtr->flags = 0;
    gridPtr->sticky = 0;
    gridPtr->size = 0;
    gridPtr->in = NULL;
    Tcl_SetHashValue(hPtr, gridPtr);
    Tk_CreateEventHandler(tkwin, StructureNotifyMask,
	    GridStructureProc, gridPtr);
    return gridPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * SetGridSize --
 *
 *	This internal procedure sets the size of the grid occupied by content.
 *
 * Results:
 *	None
 *
 * Side effects:
 *	The width and height arguments are filled in the container data
 *	structure. Additional space is allocated for the constraints to
 *	accommodate the offsets.
 *
 *----------------------------------------------------------------------
 */

static void
SetGridSize(
    Gridder *containerPtr)		/* The geometry container for this grid. */
{
    Gridder *contentPtr;	/* Current content window. */
    int maxX = 0, maxY = 0;

    for (contentPtr = containerPtr->contentPtr; contentPtr != NULL;
	    contentPtr = contentPtr->nextPtr) {
	maxX = MAX(maxX, contentPtr->numCols + contentPtr->column);
	maxY = MAX(maxY, contentPtr->numRows + contentPtr->row);
    }
    containerPtr->containerDataPtr->columnEnd = maxX;
    containerPtr->containerDataPtr->rowEnd = maxY;
    CheckSlotData(containerPtr, maxX, COLUMN, CHECK_SPACE);
    CheckSlotData(containerPtr, maxY, ROW, CHECK_SPACE);
}

/*
 *----------------------------------------------------------------------
 *
 * SetContentColumn --
 *
 *	Update column data for a content, checking that MAX_ELEMENT bound
 *      is not passed.
 *
 * Results:
 *	TCL_ERROR if out of bounds, TCL_OK otherwise
 *
 * Side effects:
 *	Content fields are updated.
 *
 *----------------------------------------------------------------------
 */

static int
SetContentColumn(
    Tcl_Interp *interp,		/* Interp for error message. */
    Gridder *contentPtr,		/* Content to be updated. */
    int column,			/* New column or -1 to be unchanged. */
    int numCols)		/* New columnspan or -1 to be unchanged. */
{
    int newColumn, newNumCols, lastCol;

    newColumn = (column >= 0) ? column : contentPtr->column;
    newNumCols = (numCols >= 1) ? numCols : contentPtr->numCols;

    lastCol = ((newColumn >= 0) ? newColumn : 0) + newNumCols;
    if (lastCol >= MAX_ELEMENT) {
	Tcl_SetObjResult(interp, Tcl_NewStringObj("column out of bounds", TCL_INDEX_NONE));
	Tcl_SetErrorCode(interp, "TK", "GRID", "BAD_COLUMN", (char *)NULL);
	return TCL_ERROR;
    }

    contentPtr->column = newColumn;
    contentPtr->numCols = newNumCols;
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * SetContentRow --
 *
 *	Update row data for a content, checking that MAX_ELEMENT bound
 *      is not passed.
 *
 * Results:
 *	TCL_ERROR if out of bounds, TCL_OK otherwise
 *
 * Side effects:
 *	Content fields are updated.
 *
 *----------------------------------------------------------------------
 */

static int
SetContentRow(
    Tcl_Interp *interp,		/* Interp for error message. */
    Gridder *contentPtr,		/* Content to be updated. */
    int row,			/* New row or -1 to be unchanged. */
    int numRows)		/* New rowspan or -1 to be unchanged. */
{
    int newRow, newNumRows, lastRow;

    newRow = (row >= 0) ? row : contentPtr->row;
    newNumRows = (numRows >= 1) ? numRows : contentPtr->numRows;

    lastRow = ((newRow >= 0) ? newRow : 0) + newNumRows;
    if (lastRow >= MAX_ELEMENT) {
	Tcl_SetObjResult(interp, Tcl_NewStringObj("row out of bounds", TCL_INDEX_NONE));
	Tcl_SetErrorCode(interp, "TK", "GRID", "BAD_ROW", (char *)NULL);
	return TCL_ERROR;
    }

    contentPtr->row = newRow;
    contentPtr->numRows = newNumRows;
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * CheckSlotData --
 *
 *	This internal procedure is used to manage the storage for row and
 *	column (slot) constraints.
 *
 * Results:
 *	TRUE if the index is OK, False otherwise.
 *
 * Side effects:
 *	A new container grid structure may be created. If so, then it is
 *	initialized. In addition, additional storage for a row or column
 *	constraints may be allocated, and the constraint maximums are
 *	adjusted.
 *
 *----------------------------------------------------------------------
 */

static int
CheckSlotData(
    Gridder *containerPtr,		/* The geometry container for this grid. */
    Tcl_Size slot,			/* Which slot to look at. */
    int slotType,		/* ROW or COLUMN. */
    int checkOnly)		/* Don't allocate new space if true. */
{
    Tcl_Size numSlot;		/* Number of slots already allocated (Space) */
    int end;			/* Last used constraint. */

    /*
     * If slot is out of bounds, return immediately.
     */

    if (slot < 0 || slot >= MAX_ELEMENT) {
	return TCL_ERROR;
    }

    if ((checkOnly == CHECK_ONLY) && (containerPtr->containerDataPtr == NULL)) {
	return TCL_ERROR;
    }

    /*
     * If we need to allocate more space, allocate a little extra to avoid
     * repeated re-alloc's for large tables. We need enough space to hold all
     * of the offsets as well.
     */

    InitContainerData(containerPtr);
    end = (slotType == ROW) ? containerPtr->containerDataPtr->rowMax :
	    containerPtr->containerDataPtr->columnMax;
    if (checkOnly == CHECK_ONLY) {
	return ((end < slot) ? TCL_ERROR : TCL_OK);
    } else {
	numSlot = (slotType == ROW) ? containerPtr->containerDataPtr->rowSpace
		: containerPtr->containerDataPtr->columnSpace;
	if (slot >= numSlot) {
	    int newNumSlot = slot + PREALLOC;
	    size_t oldSize = numSlot * sizeof(SlotInfo);
	    size_t newSize = newNumSlot * sizeof(SlotInfo);
	    SlotInfo *newSI = (SlotInfo *)ckalloc(newSize);
	    SlotInfo *oldSI = (slotType == ROW)
		    ? containerPtr->containerDataPtr->rowPtr
		    : containerPtr->containerDataPtr->columnPtr;

	    memcpy(newSI, oldSI, oldSize);
	    memset(newSI+numSlot, 0, newSize - oldSize);
	    ckfree(oldSI);
	    if (slotType == ROW) {
		containerPtr->containerDataPtr->rowPtr = newSI;
		containerPtr->containerDataPtr->rowSpace = newNumSlot;
	    } else {
		containerPtr->containerDataPtr->columnPtr = newSI;
		containerPtr->containerDataPtr->columnSpace = newNumSlot;
	    }
	}
	if (slot >= end && checkOnly != CHECK_SPACE) {
	    if (slotType == ROW) {
		containerPtr->containerDataPtr->rowMax = slot+1;
	    } else {
		containerPtr->containerDataPtr->columnMax = slot+1;
	    }
	}
	return TCL_OK;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * InitContainerData --
 *
 *	This internal procedure is used to allocate and initialize the data
 *	for a geometry container, if the data doesn't exist already.
 *
 * Results:
 *	none
 *
 * Side effects:
 *	A new container grid structure may be created. If so, then it is
 *	initialized.
 *
 *----------------------------------------------------------------------
 */

static void
InitContainerData(
    Gridder *containerPtr)
{
    if (containerPtr->containerDataPtr == NULL) {
	GridContainer *gridPtr = containerPtr->containerDataPtr = (GridContainer *)
		ckalloc(sizeof(GridContainer));
	size_t size = sizeof(SlotInfo) * TYPICAL_SIZE;

	gridPtr->columnEnd = 0;
	gridPtr->columnMax = 0;
	gridPtr->columnPtr = (SlotInfo *)ckalloc(size);
	gridPtr->columnSpace = TYPICAL_SIZE;
	gridPtr->rowEnd = 0;
	gridPtr->rowMax = 0;
	gridPtr->rowPtr = (SlotInfo *)ckalloc(size);
	gridPtr->rowSpace = TYPICAL_SIZE;
	gridPtr->startX = 0;
	gridPtr->startY = 0;
	gridPtr->anchor = GRID_DEFAULT_ANCHOR;

	memset(gridPtr->columnPtr, 0, size);
	memset(gridPtr->rowPtr, 0, size);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Unlink --
 *
 *	Remove a grid from its container's list of content.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The container will be scheduled for re-arranging, and the size of the
 *	grid will be adjusted accordingly
 *
 *----------------------------------------------------------------------
 */

static void
Unlink(
    Gridder *contentPtr)	/* Window to unlink. */
{
    Gridder *containerPtr, *contentPtr2;

    containerPtr = contentPtr->containerPtr;
    if (containerPtr == NULL) {
	return;
    }

    if (containerPtr->contentPtr == contentPtr) {
	containerPtr->contentPtr = contentPtr->nextPtr;
    } else {
	for (contentPtr2=containerPtr->contentPtr ; ; contentPtr2=contentPtr2->nextPtr) {
	    if (contentPtr2 == NULL) {
		Tcl_Panic("Unlink couldn't find previous window");
	    }
	    if (contentPtr2->nextPtr == contentPtr) {
		contentPtr2->nextPtr = contentPtr->nextPtr;
		break;
	    }
	}
    }
    if (!(containerPtr->flags & REQUESTED_RELAYOUT)) {
	containerPtr->flags |= REQUESTED_RELAYOUT;
	Tcl_DoWhenIdle(ArrangeGrid, containerPtr);
    }
    if (containerPtr->abortPtr != NULL) {
	*containerPtr->abortPtr = 1;
    }

    SetGridSize(contentPtr->containerPtr);
    contentPtr->containerPtr = NULL;

    /*
     * If we have emptied this container from content it means we are no longer
     * handling it and should mark it as free.
     *
     * Send the event "NoManagedChild" to the container to inform it about there
     * being no managed children inside it.
     */

    if ((containerPtr->contentPtr == NULL) && (containerPtr->flags & ALLOCED_CONTAINER)) {
	TkFreeGeometryContainer(containerPtr->tkwin, "grid");
	containerPtr->flags &= ~ALLOCED_CONTAINER;
	Tk_SendVirtualEvent(containerPtr->tkwin, "NoManagedChild", NULL);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * DestroyGrid --
 *
 *	This procedure is invoked by Tcl_EventuallyFree or Tcl_Release to
 *	clean up the internal structure of a grid at a safe time (when no-one
 *	is using it anymore). Cleaning up the grid involves freeing the main
 *	structure for all windows and the container structure for geometry
 *	managers.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Everything associated with the grid is freed up.
 *
 *----------------------------------------------------------------------
 */

static void
DestroyGrid(
    void *memPtr)		/* Info about window that is now dead. */
{
    Gridder *gridPtr = (Gridder *)memPtr;

    if (gridPtr->flags & REQUESTED_RELAYOUT) {
	Tcl_CancelIdleCall(ArrangeGrid, gridPtr);
    }
    if (gridPtr->containerDataPtr != NULL) {
	if (gridPtr->containerDataPtr->rowPtr != NULL) {
	    ckfree(gridPtr->containerDataPtr -> rowPtr);
	}
	if (gridPtr->containerDataPtr->columnPtr != NULL) {
	    ckfree(gridPtr->containerDataPtr -> columnPtr);
	}
	ckfree(gridPtr->containerDataPtr);
    }
    if (gridPtr->in != NULL) {
	Tcl_DecrRefCount(gridPtr->in);
    }
    ckfree(gridPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * GridStructureProc --
 *
 *	This procedure is invoked by the Tk event dispatcher in response to
 *	StructureNotify events.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	If a window was just deleted, clean up all its grid-related
 *	information. If it was just resized, re-configure its content, if any.
 *
 *----------------------------------------------------------------------
 */

static void
GridStructureProc(
    void *clientData,	/* Our information about window referred to by
				 * eventPtr. */
    XEvent *eventPtr)		/* Describes what just happened. */
{
    Gridder *gridPtr = (Gridder *)clientData;
    TkDisplay *dispPtr = ((TkWindow *) gridPtr->tkwin)->dispPtr;

    if (eventPtr->type == ConfigureNotify) {
	if ((gridPtr->contentPtr != NULL)
		&& !(gridPtr->flags & REQUESTED_RELAYOUT)) {
	    gridPtr->flags |= REQUESTED_RELAYOUT;
	    Tcl_DoWhenIdle(ArrangeGrid, gridPtr);
	}
	if ((gridPtr->containerPtr != NULL) &&
		(gridPtr->doubleBw != 2*Tk_Changes(gridPtr->tkwin)->border_width)) {
	    if (!(gridPtr->containerPtr->flags & REQUESTED_RELAYOUT)) {
		gridPtr->doubleBw = 2*Tk_Changes(gridPtr->tkwin)->border_width;
		gridPtr->containerPtr->flags |= REQUESTED_RELAYOUT;
		Tcl_DoWhenIdle(ArrangeGrid, gridPtr->containerPtr);
	    }
	}
    } else if (eventPtr->type == DestroyNotify) {
	Gridder *contentPtr, *nextPtr;

	if (gridPtr->containerPtr != NULL) {
	    Unlink(gridPtr);
	}
	for (contentPtr = gridPtr->contentPtr; contentPtr != NULL;
		contentPtr = nextPtr) {
	    Tk_ManageGeometry(contentPtr->tkwin, NULL, NULL);
	    Tk_UnmapWindow(contentPtr->tkwin);
	    contentPtr->containerPtr = NULL;
	    nextPtr = contentPtr->nextPtr;
	    contentPtr->nextPtr = NULL;
	}
	Tcl_DeleteHashEntry(Tcl_FindHashEntry(&dispPtr->gridHashTable,
		gridPtr->tkwin));
	if (gridPtr->flags & REQUESTED_RELAYOUT) {
	    Tcl_CancelIdleCall(ArrangeGrid, gridPtr);
	}
	gridPtr->tkwin = NULL;
	Tcl_EventuallyFree(gridPtr, DestroyGrid);
    } else if (eventPtr->type == MapNotify) {
	if ((gridPtr->contentPtr != NULL)
		&& !(gridPtr->flags & REQUESTED_RELAYOUT)) {
	    gridPtr->flags |= REQUESTED_RELAYOUT;
	    Tcl_DoWhenIdle(ArrangeGrid, gridPtr);
	}
    } else if (eventPtr->type == UnmapNotify) {
	Gridder *contentPtr;

	for (contentPtr = gridPtr->contentPtr; contentPtr != NULL;
		contentPtr = contentPtr->nextPtr) {
	    Tk_UnmapWindow(contentPtr->tkwin);
	}
    }
}

/*
 *----------------------------------------------------------------------
 *
 * ConfigureContent --
 *
 *	This implements the guts of the "grid configure" command. Given a list
 *	of content and configuration options, it arranges for the grid to
 *	manage the content and sets the specified options. Arguments consist
 *	of windows or window shortcuts followed by "-option value" pairs.
 *
 * Results:
 *	TCL_OK is returned if all went well. Otherwise, TCL_ERROR is returned
 *	and the interp's result is set to contain an error message.
 *
 * Side effects:
 *	Content windows get taken over by the grid.
 *
 *----------------------------------------------------------------------
 */

static int
ConfigureContent(
    Tcl_Interp *interp,		/* Interpreter for error reporting. */
    Tk_Window tkwin,		/* Any window in application containing
				 * content. Used to look up content names. */
    Tcl_Size objc,			/* Number of elements in argv. */
    Tcl_Obj *const objv[])	/* Argument objects: contains one or more
				 * window names followed by any number of
				 * "option value" pairs. Caller must make sure
				 * that there is at least one window name. */
{
    Gridder *containerPtr = NULL;
    Gridder *contentPtr;
    Tk_Window other, content, parent, ancestor;
    TkWindow *container;
    int i, j, tmp;
    int numWindows;
    int width;
    int defaultRow = -1;
    int defaultColumn = 0;	/* Default column number */
    int defaultColumnSpan = 1;	/* Default number of columns */
    const char *lastWindow;	/* Use this window to base current row/col
				 * on */
    int numSkip;		/* Number of 'x' found */
    static const char *const optionStrings[] = {
	"-column", "-columnspan", "-in", "-ipadx", "-ipady",
	"-padx", "-pady", "-row", "-rowspan", "-sticky", NULL
    };
    enum options {
	CONF_COLUMN, CONF_COLUMNSPAN, CONF_IN, CONF_IPADX, CONF_IPADY,
	CONF_PADX, CONF_PADY, CONF_ROW, CONF_ROWSPAN, CONF_STICKY };
    int index;
    const char *string;
    char firstChar;
    int positionGiven;

    /*
     * Count the number of windows, or window short-cuts.
     */

    firstChar = 0;
    for (numWindows=0, i=0; i < objc; i++) {
	Tcl_Size length;
	char prevChar = firstChar;

	string = Tcl_GetStringFromObj(objv[i], &length);
	firstChar = string[0];

	if (firstChar == '.') {
	    /*
	     * Check that windows are valid, and locate the first content's
	     * parent window (default for -in).
	     */

	    if (TkGetWindowFromObj(interp, tkwin, objv[i], &content) != TCL_OK) {
		return TCL_ERROR;
	    }
	    if (containerPtr == NULL) {
		/*
		 * Is there any saved -in from a removed content?
		 * If there is, it becomes default for -in.
		 * If the stored container does not exist, just ignore it.
		 */

		if (!(contentPtr = GetGrid(content))) {
		    continue;
		}
		if (contentPtr->in != NULL) {
		    if (TkGetWindowFromObj(interp, content, contentPtr->in, &parent)
			    == TCL_OK) {
			if (!(containerPtr = GetGrid(parent))) {
			    continue;
			}
			InitContainerData(containerPtr);
		    }
		}
	    }
	    if (containerPtr == NULL) {
		parent = Tk_Parent(content);
		if (parent != NULL) {
		    if (!(containerPtr = GetGrid(parent))) {
			continue;
		    }
		    InitContainerData(containerPtr);
		}
	    }
	    numWindows++;
	    continue;
	}
	if (length > 1 && i == 0) {
	    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
		    "bad argument \"%s\": must be name of window", string));
	    Tcl_SetErrorCode(interp, "TK", "GRID", "BAD_PARAMETER", (char *)NULL);
	    return TCL_ERROR;
	}
	if (length > 1 && firstChar == '-') {
	    break;
	}
	if (length > 1) {
	    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
		    "unexpected parameter \"%s\" in configure list:"
		    " should be window name or option", string));
	    Tcl_SetErrorCode(interp, "TK", "GRID", "BAD_PARAMETER", (char *)NULL);
	    return TCL_ERROR;
	}

	if ((firstChar == REL_HORIZ) && ((numWindows == 0) ||
		(prevChar == REL_SKIP) || (prevChar == REL_VERT))) {
	    Tcl_SetObjResult(interp, Tcl_NewStringObj(
		    "must specify window before shortcut '-'", TCL_INDEX_NONE));
	    Tcl_SetErrorCode(interp, "TK", "GRID", "SHORTCUT_USAGE", (char *)NULL);
	    return TCL_ERROR;
	}

	if ((firstChar == REL_VERT) || (firstChar == REL_SKIP)
		|| (firstChar == REL_HORIZ)) {
	    continue;
	}

	Tcl_SetObjResult(interp, Tcl_ObjPrintf(
		"invalid window shortcut, \"%s\" should be '-', 'x', or '^'",
		string));
	Tcl_SetErrorCode(interp, "TK", "GRID", "SHORTCUT_USAGE", (char *)NULL);
	return TCL_ERROR;
    }
    numWindows = i;

    if ((objc - numWindows) & 1) {
	Tcl_SetObjResult(interp, Tcl_NewStringObj(
		"extra option or option with no value", TCL_INDEX_NONE));
	Tcl_SetErrorCode(interp, "TK", "GRID", "BAD_PARAMETER", (char *)NULL);
	return TCL_ERROR;
    }

    /*
     * Go through all options looking for -in and -row, which are needed to be
     * found first to handle the special case where ^ is used on a row without
     * windows names, but with an -in option. Since all options are checked
     * here, we do not need to handle the error case again later.
     */

    for (i = numWindows; i < objc; i += 2) {
	if (Tcl_GetIndexFromObjStruct(interp, objv[i], optionStrings,
		sizeof(char *), "option", 0, &index) != TCL_OK) {
	    return TCL_ERROR;
	}
	if (index == CONF_IN) {
	    if (TkGetWindowFromObj(interp, tkwin, objv[i+1], &other) !=
		    TCL_OK) {
		return TCL_ERROR;
	    }
	    if (!(containerPtr = GetGrid(other))) {
		continue;
	    }
	    InitContainerData(containerPtr);
	} else if (index == CONF_ROW) {
	    if (Tcl_GetIntFromObj(interp, objv[i+1], &tmp) != TCL_OK
		    || tmp < 0) {
		Tcl_SetObjResult(interp, Tcl_ObjPrintf(
			"bad row value \"%s\": must be a non-negative integer",
			Tcl_GetString(objv[i+1])));
		Tcl_SetErrorCode(interp, "TK", "VALUE", "POSITIVE_INT", (char *)NULL);
		return TCL_ERROR;
	    }
	    defaultRow = tmp;
	}
    }

    /*
     * If no -row is given, use the next row after the highest occupied row
     * of the container.
     */

    if (defaultRow < 0) {
	if (containerPtr != NULL && containerPtr->containerDataPtr != NULL) {
	    SetGridSize(containerPtr);
	    defaultRow = containerPtr->containerDataPtr->rowEnd;
	} else {
	    defaultRow = 0;
	}
    }

    /*
     * Iterate over all of the content windows and short-cuts, parsing options
     * for each content. It's a bit wasteful to re-parse the options for each
     * content, but things get too messy if we try to parse the arguments just
     * once at the beginning. For example, if a content already is managed we
     * want to just change a few existing values without resetting everything.
     * If there are multiple windows, the -in option only gets processed for
     * the first window.
     */

    positionGiven = 0;
    for (j = 0; j < numWindows; j++) {
	string = Tcl_GetString(objv[j]);
	firstChar = string[0];

	/*
	 * '^' and 'x' cause us to skip a column. '-' is processed as part of
	 * its preceeding content.
	 */

	if ((firstChar == REL_VERT) || (firstChar == REL_SKIP)) {
	    defaultColumn++;
	    continue;
	}
	if (firstChar == REL_HORIZ) {
	    continue;
	}

	for (defaultColumnSpan = 1; j + defaultColumnSpan < numWindows;
		defaultColumnSpan++) {
	    string = Tcl_GetString(objv[j + defaultColumnSpan]);

	    if (*string != REL_HORIZ) {
		break;
	    }
	}

	if (TkGetWindowFromObj(interp, tkwin, objv[j], &content) != TCL_OK) {
	    return TCL_ERROR;
	}

	if (Tk_TopWinHierarchy(content)) {
	    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
		    "can't manage \"%s\": it's a top-level window",
		    Tcl_GetString(objv[j])));
	    Tcl_SetErrorCode(interp, "TK", "GEOMETRY", "TOPLEVEL", (char *)NULL);
	    return TCL_ERROR;
	}
	if (!(contentPtr = GetGrid(content))) {
	    continue;
	}

	/*
	 * The following statement is taken from tkPack.c:
	 *
	 * "If the content isn't currently managed, reset all of its
	 * configuration information to default values (there could be old
	 * values left from a previous packer)."
	 *
	 * I [D.S.] disagree with this statement. If a content is disabled
	 * (using "forget") and then re-enabled, I submit that 90% of the time
	 * the programmer will want it to retain its old configuration
	 * information. If the programmer doesn't want this behavior, then the
	 * defaults can be reestablished by hand, without having to worry
	 * about keeping track of the old state.
	 */

	for (i = numWindows; i < objc; i += 2) {
	    Tcl_GetIndexFromObjStruct(interp, objv[i], optionStrings,
		    sizeof(char *), "option", 0, &index);
	    switch ((enum options) index) {
	    case CONF_COLUMN:
		if (Tcl_GetIntFromObj(NULL, objv[i+1], &tmp) != TCL_OK
			|| tmp < 0) {
		    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
			    "bad column value \"%s\": must be a non-negative integer",
			    Tcl_GetString(objv[i+1])));
		    Tcl_SetErrorCode(interp, "TK", "VALUE", "COLUMN", (char *)NULL);
		    return TCL_ERROR;
		}
		if (SetContentColumn(interp, contentPtr, tmp, -1) != TCL_OK) {
		    return TCL_ERROR;
		}
		break;
	    case CONF_COLUMNSPAN:
		if (Tcl_GetIntFromObj(NULL, objv[i+1], &tmp) != TCL_OK
			|| tmp <= 0) {
		    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
			    "bad columnspan value \"%s\": must be a positive integer",
			    Tcl_GetString(objv[i+1])));
		    Tcl_SetErrorCode(interp, "TK", "VALUE", "SPAN", (char *)NULL);
		    return TCL_ERROR;
		}
		if (SetContentColumn(interp, contentPtr, -1, tmp) != TCL_OK) {
		    return TCL_ERROR;
		}
		break;
	    case CONF_IN:
		if (TkGetWindowFromObj(interp, tkwin, objv[i+1],
			&other) != TCL_OK) {
		    return TCL_ERROR;
		}
		if (other == content) {
		    Tcl_SetObjResult(interp, Tcl_NewStringObj(
			    "window can't be managed in itself", TCL_INDEX_NONE));
		    Tcl_SetErrorCode(interp, "TK", "GEOMETRY", "SELF", (char *)NULL);
		    return TCL_ERROR;
		}
		positionGiven = 1;
		if (!(containerPtr = GetGrid(other))) {
		    continue;
		}
		InitContainerData(containerPtr);
		break;
	    case CONF_STICKY: {
		int sticky = StringToSticky(Tcl_GetString(objv[i+1]));

		if (sticky == -1) {
		    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
			    "bad stickyness value \"%s\": must be"
			    " a string containing n, e, s, and/or w",
			    Tcl_GetString(objv[i+1])));
		    Tcl_SetErrorCode(interp, "TK", "VALUE", "STICKY", (char *)NULL);
		    return TCL_ERROR;
		}
		contentPtr->sticky = sticky;
		break;
	    }
	    case CONF_IPADX:
		if ((Tk_GetPixelsFromObj(NULL, content, objv[i+1],
			&tmp) != TCL_OK) || (tmp < 0)) {
		    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
			    "bad ipadx value \"%s\": must be positive screen distance",
			    Tcl_GetString(objv[i+1])));
		    Tcl_SetErrorCode(interp, "TK", "VALUE", "INT_PAD", (char *)NULL);
		    return TCL_ERROR;
		}
		contentPtr->iPadX = tmp * 2;
		break;
	    case CONF_IPADY:
		if ((Tk_GetPixelsFromObj(NULL, content, objv[i+1],
			&tmp) != TCL_OK) || (tmp < 0)) {
		    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
			    "bad ipady value \"%s\": must be positive screen distance",
			    Tcl_GetString(objv[i+1])));
		    Tcl_SetErrorCode(interp, "TK", "VALUE", "INT_PAD", (char *)NULL);
		    return TCL_ERROR;
		}
		contentPtr->iPadY = tmp * 2;
		break;
	    case CONF_PADX:
		if (TkParsePadAmount(interp, tkwin, objv[i+1],
			&contentPtr->padLeft, &contentPtr->padX) != TCL_OK) {
		    return TCL_ERROR;
		}
		break;
	    case CONF_PADY:
		if (TkParsePadAmount(interp, tkwin, objv[i+1],
			&contentPtr->padTop, &contentPtr->padY) != TCL_OK) {
		    return TCL_ERROR;
		}
		break;
	    case CONF_ROW:
		if (Tcl_GetIntFromObj(NULL, objv[i+1], &tmp) != TCL_OK
			|| tmp < 0) {
		    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
			    "bad row value \"%s\": must be a non-negative integer",
			    Tcl_GetString(objv[i+1])));
		    Tcl_SetErrorCode(interp, "TK", "VALUE", "COLUMN", (char *)NULL);
		    return TCL_ERROR;
		}
		if (SetContentRow(interp, contentPtr, tmp, -1) != TCL_OK) {
		    return TCL_ERROR;
		}
		break;
	    case CONF_ROWSPAN:
		if ((Tcl_GetIntFromObj(NULL, objv[i+1], &tmp) != TCL_OK)
			|| tmp <= 0) {
		    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
			    "bad rowspan value \"%s\": must be a positive integer",
			    Tcl_GetString(objv[i+1])));
		    Tcl_SetErrorCode(interp, "TK", "VALUE", "SPAN", (char *)NULL);
		    return TCL_ERROR;
		}
		if (SetContentRow(interp, contentPtr, -1, tmp) != TCL_OK) {
		    return TCL_ERROR;
		}
		break;
	    }
	}

	/*
	 * If no position was specified via -in and the content is already
	 * packed, then leave it in its current location.
	 */

	if (!positionGiven && (contentPtr->containerPtr != NULL)) {
	    containerPtr = contentPtr->containerPtr;
	    goto scheduleLayout;
	}

	/*
	 * If the same -in window is passed in again, then just leave it in
	 * its current location.
	 */

	if (positionGiven && (containerPtr == contentPtr->containerPtr)) {
	    goto scheduleLayout;
	}

	/*
	 * Make sure we have a geometry container. We look at:
	 *  1)   the -in flag
	 *  2)   the parent of the first content.
	 */

	parent = Tk_Parent(content);
	if (containerPtr == NULL) {
	    if (!(containerPtr = GetGrid(parent))) {
		continue;
	    }
	    InitContainerData(containerPtr);
	}

	if (contentPtr->containerPtr != NULL && contentPtr->containerPtr != containerPtr) {
	    if (contentPtr->containerPtr->tkwin != Tk_Parent(contentPtr->tkwin)) {
		Tk_UnmaintainGeometry(contentPtr->tkwin, contentPtr->containerPtr->tkwin);
	    }
	    Unlink(contentPtr);
	    contentPtr->containerPtr = NULL;
	}

	if (contentPtr->containerPtr == NULL) {
	    Gridder *tempPtr = containerPtr->contentPtr;

	    contentPtr->containerPtr = containerPtr;
	    containerPtr->contentPtr = contentPtr;
	    contentPtr->nextPtr = tempPtr;
	}

	/*
	 * Make sure that the content's parent is either the container or an
	 * ancestor of the container, and that the container and content aren't the
	 * same.
	 */

	for (ancestor = containerPtr->tkwin; ; ancestor = Tk_Parent(ancestor)) {
	    if (ancestor == parent) {
		break;
	    }
	    if (Tk_TopWinHierarchy(ancestor)) {
		Tcl_SetObjResult(interp, Tcl_ObjPrintf(
			"can't put \"%s\" inside \"%s\"", Tcl_GetString(objv[j]),
			Tk_PathName(containerPtr->tkwin)));
		Tcl_SetErrorCode(interp, "TK", "GEOMETRY", "HIERARCHY", (char *)NULL);
		Unlink(contentPtr);
		return TCL_ERROR;
	    }
	}

	/*
	 * Check for management loops.
	 */

	for (container = (TkWindow *)containerPtr->tkwin; container != NULL;
	     container = (TkWindow *)TkGetContainer(container)) {
	    if (container == (TkWindow *)content) {
		Tcl_SetObjResult(interp, Tcl_ObjPrintf(
		    "can't put \"%s\" inside \"%s\": would cause management loop",
		    Tcl_GetString(objv[j]), Tk_PathName(containerPtr->tkwin)));
		Tcl_SetErrorCode(interp, "TK", "GEOMETRY", "LOOP", (char *)NULL);
		Unlink(contentPtr);
		return TCL_ERROR;
	    }
	}
	if (containerPtr->tkwin != Tk_Parent(content)) {
	    ((TkWindow *)content)->maintainerPtr = (TkWindow *)containerPtr->tkwin;
	}

	Tk_ManageGeometry(content, &gridMgrType, contentPtr);

	if (!(containerPtr->flags & DONT_PROPAGATE)) {
	    if (TkSetGeometryContainer(interp, containerPtr->tkwin, "grid")
		    != TCL_OK) {
		Tk_ManageGeometry(content, NULL, NULL);
		Unlink(contentPtr);
		return TCL_ERROR;
	    }
	    containerPtr->flags |= ALLOCED_CONTAINER;
	}

	/*
	 * Assign default position information.
	 */

	if (contentPtr->column == -1) {
	    if (SetContentColumn(interp, contentPtr, defaultColumn,-1) != TCL_OK){
		return TCL_ERROR;
	    }
	}
	if (SetContentColumn(interp, contentPtr, -1,
		contentPtr->numCols + defaultColumnSpan - 1) != TCL_OK) {
	    return TCL_ERROR;
	}
	if (contentPtr->row == -1) {
	    if (SetContentRow(interp, contentPtr, defaultRow, -1) != TCL_OK) {
		return TCL_ERROR;
	    }
	}
	defaultColumn += contentPtr->numCols;
	defaultColumnSpan = 1;

	/*
	 * Arrange for the container to be re-arranged at the first idle moment.
	 */

    scheduleLayout:
	if (containerPtr->abortPtr != NULL) {
	    *containerPtr->abortPtr = 1;
	}
	if (!(containerPtr->flags & REQUESTED_RELAYOUT)) {
	    containerPtr->flags |= REQUESTED_RELAYOUT;
	    Tcl_DoWhenIdle(ArrangeGrid, containerPtr);
	}
    }

    /*
     * Now look for all the "^"'s.
     */

    lastWindow = NULL;
    numSkip = 0;
    for (j = 0; j < numWindows; j++) {
	struct Gridder *otherPtr;
	int match;			/* Found a match for the ^ */
	int lastRow, lastColumn;	/* Implied end of table. */

	string = Tcl_GetString(objv[j]);
	firstChar = string[0];

	if (firstChar == '.') {
	    lastWindow = string;
	    numSkip = 0;
	}
	if (firstChar == REL_SKIP) {
	    numSkip++;
	}
	if (firstChar != REL_VERT) {
	    continue;
	}

	if (containerPtr == NULL) {
	    Tcl_SetObjResult(interp, Tcl_NewStringObj(
		    "can't use '^', can't find container window", TCL_INDEX_NONE));
	    Tcl_SetErrorCode(interp, "TK", "GRID", "SHORTCUT_USAGE", (char *)NULL);
	    return TCL_ERROR;
	}

	/*
	 * Count the number of consecutive ^'s starting from this position.
	 */

	for (width = 1; width + j < numWindows; width++) {
	    string = Tcl_GetString(objv[j+width]);

	    if (*string != REL_VERT) {
		break;
	    }
	}

	/*
	 * Find the implied grid location of the ^
	 */

	if (lastWindow == NULL) {
	    lastRow = defaultRow - 1;
	    lastColumn = 0;
	} else {
	    other = Tk_NameToWindow(interp, lastWindow, tkwin);
	    if (!(otherPtr = GetGrid(other))) {
		continue;
	    }
	    lastRow = otherPtr->row + otherPtr->numRows - 2;
	    lastColumn = otherPtr->column + otherPtr->numCols;
	}

	lastColumn += numSkip;

	match = 0;
	for (contentPtr = containerPtr->contentPtr; contentPtr != NULL;
		contentPtr = contentPtr->nextPtr) {

	    if (contentPtr->column == lastColumn
		    && contentPtr->row + contentPtr->numRows - 1 == lastRow) {
		if (contentPtr->numCols <= width) {
		    if (SetContentRow(interp, contentPtr, -1,
			    contentPtr->numRows + 1) != TCL_OK) {
			return TCL_ERROR;
		    }
		    match++;
		    j += contentPtr->numCols - 1;
		    lastWindow = Tk_PathName(contentPtr->tkwin);
		    numSkip = 0;
		    break;
		}
	    }
	}
	if (!match) {
	    Tcl_SetObjResult(interp, Tcl_NewStringObj(
		    "can't find content to extend with \"^\"", TCL_INDEX_NONE));
	    Tcl_SetErrorCode(interp, "TK", "GRID", "SHORTCUT_USAGE", (char *)NULL);
	    return TCL_ERROR;
	}
    }

    if (containerPtr == NULL) {
	Tcl_SetObjResult(interp, Tcl_NewStringObj(
		"can't determine container window", TCL_INDEX_NONE));
	Tcl_SetErrorCode(interp, "TK", "GRID", "SHORTCUT_USAGE", (char *)NULL);
	return TCL_ERROR;
    }
    SetGridSize(containerPtr);

    /*
     * If we have emptied this container from content it means we are no longer
     * handling it and should mark it as free.
     *
     * Send the event "NoManagedChild" to the container to inform it about there
     * being no managed children inside it.
     */

    if (containerPtr->contentPtr == NULL && containerPtr->flags & ALLOCED_CONTAINER) {
	TkFreeGeometryContainer(containerPtr->tkwin, "grid");
	containerPtr->flags &= ~ALLOCED_CONTAINER;
	Tk_SendVirtualEvent(containerPtr->tkwin, "NoManagedChild", NULL);
    }

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * StickyToObj
 *
 *	Converts the internal boolean combination of "sticky" bits onto a Tcl
 *	list element containing zero or more of n, s, e, or w.
 *
 * Results:
 *	A new object is returned that holds the sticky representation.
 *
 * Side effects:
 *	none.
 *
 *----------------------------------------------------------------------
 */

static Tcl_Obj *
StickyToObj(
    int flags)			/* The sticky flags. */
{
    int count = 0;
    char buffer[4];

    if (flags & STICK_NORTH) {
	buffer[count++] = 'n';
    }
    if (flags & STICK_EAST) {
	buffer[count++] = 'e';
    }
    if (flags & STICK_SOUTH) {
	buffer[count++] = 's';
    }
    if (flags & STICK_WEST) {
	buffer[count++] = 'w';
    }
    return Tcl_NewStringObj(buffer, count);
}

/*
 *----------------------------------------------------------------------
 *
 * StringToSticky --
 *
 *	Converts an ascii string representing a widgets stickyness into the
 *	boolean result.
 *
 * Results:
 *	The boolean combination of the "sticky" bits is retuned. If an error
 *	occurs, such as an invalid character, -1 is returned instead.
 *
 * Side effects:
 *	none
 *
 *----------------------------------------------------------------------
 */

static int
StringToSticky(
    const char *string)
{
    int sticky = 0;
    char c;

    while ((c = *string++) != '\0') {
	switch (c) {
	case 'n': case 'N':
	    sticky |= STICK_NORTH;
	    break;
	case 'e': case 'E':
	    sticky |= STICK_EAST;
	    break;
	case 's': case 'S':
	    sticky |= STICK_SOUTH;
	    break;
	case 'w': case 'W':
	    sticky |= STICK_WEST;
	    break;
	case ' ': case ',': case '\t': case '\r': case '\n':
	    break;
	default:
	    return -1;
	}
    }
    return sticky;
}

/*
 *----------------------------------------------------------------------
 *
 * NewPairObj --
 *
 *	Creates a new list object and fills it with two integer objects.
 *
 * Results:
 *	The newly created list object is returned.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static Tcl_Obj *
NewPairObj(
    Tcl_WideInt val1, Tcl_WideInt val2)
{
    Tcl_Obj *ary[2];

    ary[0] = Tcl_NewWideIntObj(val1);
    ary[1] = Tcl_NewWideIntObj(val2);
    return Tcl_NewListObj(2, ary);
}

/*
 *----------------------------------------------------------------------
 *
 * NewQuadObj --
 *
 *	Creates a new list object and fills it with four integer objects.
 *
 * Results:
 *	The newly created list object is returned.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static Tcl_Obj *
NewQuadObj(
    Tcl_WideInt val1, Tcl_WideInt val2, Tcl_WideInt val3, Tcl_WideInt val4)
{
    Tcl_Obj *ary[4];

    ary[0] = Tcl_NewWideIntObj(val1);
    ary[1] = Tcl_NewWideIntObj(val2);
    ary[2] = Tcl_NewWideIntObj(val3);
    ary[3] = Tcl_NewWideIntObj(val4);
    return Tcl_NewListObj(4, ary);
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
