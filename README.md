# mandelbrot
Mandelbrot and Julia set explorer
[Download the app here.](https://github.com/shaped1/mandelbrot/blob/main/mbrot.exe?raw=true)
This is a fractal generator for Mandelbrot and Julia sets.

Click and drag a rectangle to zoom in.
Contrl-Click to zoom out.
Right-click and drag to pan to a new location.
You can also directly enter the coordinates of the center location and the zoom factor in the status window.

Checking the checkbox switches between the Mandelbrot set to the corresponding Julia set in the centered location and vice versa.
When switching to a Julia set, the view is re-centered at 0 and zoomed out.
When switching back to the Mandelbrot set, the original center and zoom factor are restored.

Saving an animated movie requires a VFW-compatible AVI codec installed.  The movie will be centered on the target coordinate and start from fully zoomed out to zoomed in at the zoom level displayed in current view.

The program uses background threading and parallel computation to determine points inside or outside the set.  Points which escape are colored via a pallete.  Points which are proven to be bound (because they cycle) are colored black.  Points in grey have yet to be determined.  You can see the current iteration depth and the number of currently unresolved pixels in the status window.

You need not wait for the computation to finish before selecting a new action.
