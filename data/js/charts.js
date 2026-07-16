// Lightweight Canvas Line Chart Engine
const historyData = { tds: [], temp: [], hum: [], wt: [], lux: [] };
const MAX_POINTS = 15; // 15 seconds of history

function drawChart(canvasId, dataArr, color = '#3b82f6') {
    const canvas = document.getElementById(canvasId);
    if (!canvas) return;

    const ctx = canvas.getContext('2d');
    // Ensure internal resolution matches CSS display size
    const w = canvas.width = canvas.offsetWidth;
    const h = canvas.height = canvas.offsetHeight;

    ctx.clearRect(0, 0, w, h);
    if (dataArr.length < 2) return;

    // Auto-scaling logic
    let max = Math.max(...dataArr);
    let min = Math.min(...dataArr);
    if (max === min) { max += 10; min -= 10; } // Prevent div by zero
    const range = max - min;

    // Draw Graph
    ctx.beginPath();
    ctx.strokeStyle = color;
    ctx.lineWidth = 3;
    ctx.lineJoin = 'round';

    for (let i = 0; i < dataArr.length; i++) {
        const x = (i / (MAX_POINTS - 1)) * w;
        // Pad 20px on top and bottom so lines don't hit the absolute edge
        const y = h - (((dataArr[i] - min) / range) * (h - 40) + 20);

        if (i === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
    }
    ctx.stroke();
}
