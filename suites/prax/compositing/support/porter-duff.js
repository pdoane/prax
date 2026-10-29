// Draws the Porter Duff pages' two rectangles on each `canvas.pd`: the destination, blue rgb(0% 40% 80% / 0.8), 90 by
// 60 at (10, 10), drawn first with source-over, then the source, orange rgb(100% 60% 0% / 0.6), 90 by 60 at (60, 30),
// drawn with the canvas's `data-op` as its globalCompositeOperation. They overlap from (60, 30) to (100, 70).
// `data-only="source"` or `"destination"` draws that rectangle alone. `data-steps` draws a list of rectangles in order,
// each `<operator> <rectangle>`, comma-separated, starting on the empty canvas. The canvas's bitmap has a pixel for
// each device pixel, so its edges fall on the guides.
const pdRectangles = {
    destination: ["rgb(0% 40% 80% / 0.8)", 10, 10],
    source: ["rgb(100% 60% 0% / 0.6)", 60, 30],
};

function pdDraw(context, operator, rectangle) {
    const [color, x, y] = pdRectangles[rectangle];
    context.globalCompositeOperation = operator;
    context.fillStyle = color;
    context.fillRect(x, y, 90, 60);
}

for (const canvas of document.querySelectorAll("canvas.pd")) {
    const ratio = window.devicePixelRatio || 1;
    canvas.width = 160 * ratio;
    canvas.height = 100 * ratio;
    const context = canvas.getContext("2d");
    context.scale(ratio, ratio);
    if (canvas.dataset.only) {
        pdDraw(context, "source-over", canvas.dataset.only);
    } else if (canvas.dataset.steps) {
        for (const step of canvas.dataset.steps.split(",")) {
            const [operator, rectangle] = step.trim().split(" ");
            pdDraw(context, operator, rectangle);
        }
    } else {
        pdDraw(context, "source-over", "destination");
        pdDraw(context, canvas.dataset.op, "source");
    }
}
