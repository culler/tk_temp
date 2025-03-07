/*
 * tkMenubutton.h --
 *
 *	Declarations of types and functions used to implement the menubutton
 *	widget.
 *
 * Copyright © 1996-1997 Sun Microsystems, Inc.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#ifndef _TKMENUBUTTON
#define _TKMENUBUTTON

#ifndef _TKINT
#include "tkInt.h"
#endif

#ifndef _TKMENU
#include "tkMenu.h"
#endif

/*
 * Legal values for the "orient" field of TkMenubutton records.
 */

enum direction {
    DIRECTION_ABOVE, DIRECTION_BELOW, DIRECTION_FLUSH,
    DIRECTION_LEFT, DIRECTION_RIGHT
};

/*
 * Legal values for the "state" field of TkMenubutton records.
 */

enum state {
    STATE_ACTIVE, STATE_DISABLED, STATE_NORMAL
};

/*
 * A data structure of the following type is kept for each widget managed by
 * this file:
 */

typedef struct {
    Tk_Window tkwin;		/* Window that embodies the widget. NULL means
				 * that the window has been destroyed but the
				 * data structures haven't yet been cleaned
				 * up. */
    Display *display;		/* Display containing widget. Needed, among
				 * other things, so that resources can bee
				 * freed up even after tkwin has gone away. */
    Tcl_Interp *interp;		/* Interpreter associated with menubutton. */
    Tcl_Command widgetCmd;	/* Token for menubutton's widget command. */
    Tk_OptionTable optionTable;	/* Table that defines configuration options
				 * available for this widget. */
    Tcl_Obj *menuNameObj;		/* Name of menu associated with widget. */

    /*
     * Information about what's displayed in the menu button:
     */

    Tcl_Obj *textObj;			/* Text to display in button. May be NULL. */
    int underline;		/* Index of character to underline. INT_MIN means no underline */
    Tcl_Obj *textVarNameObj;	/* Name of variable or NULL. If
				 * non-NULL, button displays the contents of
				 * this variable. */
    Pixmap bitmap;		/* Bitmap to display or None. If not None then
				 * text and textVar and underline are
				 * ignored. */
    Tcl_Obj *imageObj;		/* Name of image to display, or
				 * NULL. If non-NULL, bitmap, text, and
				 * textVarName are ignored. */
    Tk_Image image;		/* Image to display in window, or NULL if
				 * none. */

    /*
     * Information used when displaying widget:
     */

    enum state state;		/* State of button for display purposes:
				 * normal, active, or disabled. */
    Tk_3DBorder normalBorder;	/* Structure used to draw 3-D border and
				 * background when window isn't active. NULL
				 * means no such border exists. */
    Tk_3DBorder activeBorder;	/* Structure used to draw 3-D border and
				 * background when window is active. NULL
				 * means no such border exists. */
#if TK_MAJOR_VERSION > 8
    Tcl_Obj *borderWidthObj;	/* Width of border. */
#else
    int borderWidth;
#endif
    int relief;			/* 3-d effect: TK_RELIEF_RAISED, etc. */
#if TK_MAJOR_VERSION > 8
    Tcl_Obj *highlightWidthObj;	/* Width in pixels of highlight to draw around
				 * widget when it has the focus. 0 means
				 * don't draw a highlight. */
#else
    int highlightWidth;
#endif
    XColor *highlightBgColorPtr;/* Color for drawing traversal highlight area
				 * when highlight is off. */
    XColor *highlightColorPtr;	/* Color for drawing traversal highlight. */
    int inset;			/* Total width of all borders, including
				 * traversal highlight and 3-D border.
				 * Indicates how much interior stuff must be
				 * offset from outside edges to leave room for
				 * borders. */
    Tk_Font tkfont;		/* Information about text font, or NULL. */
    XColor *normalFg;		/* Foreground color in normal mode. */
    XColor *activeFg;		/* Foreground color in active mode. NULL means
				 * use normalFg instead. */
    XColor *disabledFg;		/* Foreground color when disabled. NULL means
				 * use normalFg with a 50% stipple instead. */
    GC normalTextGC;		/* GC for drawing text in normal mode. */
    GC activeTextGC;		/* GC for drawing text in active mode (NULL
				 * means use normalTextGC). */
    Pixmap gray;		/* Pixmap for displaying disabled text/icon if
				 * disabledFg is NULL. */
    GC disabledGC;		/* Used to produce disabled effect for
				 * text. */
    GC stippleGC;		/* Used to produce disabled stipple effect for
				 * images when disabled. */
    int leftBearing;		/* Distance from text origin to leftmost drawn
				 * pixel (positive means to right). */
    int rightBearing;		/* Amount text sticks right from its
				 * origin. */
    Tcl_Obj *widthObj;		/* Value of -width option. */
    Tcl_Obj *heightObj;		/* Value of -height option. */
    int width, height;		/* If > 0, these specify dimensions to request
				 * for window, in characters for text and in
				 * pixels for bitmaps. In this case the actual
				 * size of the text string or bitmap is
				 * ignored in computing desired window
				 * size. */
#if TK_MAJOR_VERSION > 8
    Tcl_Obj *wrapLengthObj;	/* Line length (in pixels) at which to wrap
				 * onto next line. 0 means don't wrap
				 * except at newlines. */
    Tcl_Obj *padXObj, *padYObj;	/* Extra space around text or bitmap (pixels
				 * on each side). */
#else
    int wrapLength;
    int padX, padY;
#endif
    Tk_Anchor anchor;		/* Where text/bitmap should be displayed
				 * inside window region. */
    Tk_Justify justify;		/* Justification to use for multi-line
				 * text. */
    int textWidth;		/* Width needed to display text as requested,
				 * in pixels. */
    int textHeight;		/* Height needed to display text as requested,
				 * in pixels. */
    Tk_TextLayout textLayout;	/* Saved text layout information. */
    int indicatorOn;		/* Non-zero means display indicator; 0 means
				 * don't display. */
    int indicatorHeight;	/* Height of indicator in pixels. This same
				 * amount of extra space is also left on each
				 * side of the indicator. 0 if no
				 * indicator. */
    int indicatorWidth;		/* Width of indicator in pixels, including
				 * indicatorHeight in padding on each side. 0
				 * if no indicator. */

    /*
     * Miscellaneous information:
     */

    int compound;		/* Value of -compound option; specifies
				 * whether the menubutton should show both an
				 * image and text, and, if so, how. */
    enum direction direction;	/* Direction for where to pop the menu. Valid
				 * directions are "above", "below", "flush",
				 * "left", and "right". "above" and "below"
				 * will attempt to pop the menu completely
				 * above or below the menu respectively.
				 * "flush" means that the upper left corner
				 * of the menubutton is where the menu pops up.
				 * "left" and "right" will pop the menu left
				 * or right, and the active item will be next
				 * to the button. */
    Tk_Cursor cursor;		/* Current cursor for window, or NULL. */
    Tcl_Obj *takeFocusObj;	/* Value of -takefocus option; not used in the
				 * C code, but used by keyboard traversal
				 * scripts. May be NULL. */
    int flags;			/* Various flags; see below for
				 * definitions. */
} TkMenuButton;

/*
 * Flag bits for buttons:
 *
 * REDRAW_PENDING:		Non-zero means a DoWhenIdle handler has
 *				already been queued to redraw this window.
 * POSTED:			Non-zero means that the menu associated with
 *				this button has been posted (typically because
 *				of an active button press).
 * GOT_FOCUS:			Non-zero means this button currently has the
 *				input focus.
 */

#define REDRAW_PENDING		1
#define POSTED			2
#define GOT_FOCUS		4

/*
 * The following constants define the dimensions of the cascade indicator,
 * which is displayed if the "-indicatoron" option is true. The units for
 * these options are 1/10 millimeters.
 */

#define INDICATOR_WIDTH		40
#define INDICATOR_HEIGHT	17

/*
 * Declaration of procedures used in the implementation of the button widget.
 */

MODULE_SCOPE void	TkpComputeMenuButtonGeometry(TkMenuButton *mbPtr);
MODULE_SCOPE TkMenuButton *TkpCreateMenuButton(Tk_Window tkwin);
MODULE_SCOPE void	TkpDisplayMenuButton(void *clientData);
MODULE_SCOPE void	TkpDestroyMenuButton(TkMenuButton *mbPtr);
MODULE_SCOPE void	TkMenuButtonWorldChanged(void *instanceData);

#endif /* _TKMENUBUTTON */
