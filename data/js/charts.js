/**
 * HyGrow IoT - Charting & Data Export Module
 * Handles high-performance Canvas API rendering and CSV generation.
 */

// Draws a standard single-value line chart with a gradient fill
function drawChart(context, cvs, dataArr, colorStr) {
    if(!cvs || !cvs.width || !context) return;
    const w = cvs.width / window.devicePixelRatio;
    const h = cvs.height / window.devicePixelRatio;

    context.clearRect(0, 0, w, h);

    context.beginPath();
    context.strokeStyle = colorStr;
    context.lineWidth = 4;
    context.lineCap = 'round';
    context.lineJoin = 'round';

    // Guard divide-by-zero: with exactly one buffered point, length-1 is 0
    // and step becomes Infinity, which turns every x coordinate into NaN
    // and silently draws nothing. Falling back to width for a single point
    // is an arbitrary-but-harmless choice — there's nothing to connect a
    // single point to anyway.
    const step = w / Math.max(dataArr.length - 1, 1);
    const max = Math.max(...dataArr, 10); // Minimum scale of 10 to prevent flatlining at 0

    dataArr.forEach((val, i) => {
        const x = i * step;
        const y = h - ((val / max) * h * 0.8) - 20;
        if(i === 0) context.moveTo(x, y);
        else context.lineTo(x, y);
    });

    // Soft glow effect
    context.shadowBlur = 20;
    context.shadowColor = colorStr === '#afc6ff' ? 'rgba(175, 198, 255, 0.4)' : 'rgba(78, 222, 163, 0.4)';
    context.stroke();
    context.shadowBlur = 0;

    // Fill gradient under line
    context.lineTo(w, h);
    context.lineTo(0, h);
    context.closePath();

    const gradient = context.createLinearGradient(0, 0, 0, h);
    let rgb = colorStr === '#afc6ff' ? '175, 198, 255' : '78, 222, 163';
    gradient.addColorStop(0, `rgba(${rgb}, 0.15)`);
    gradient.addColorStop(1, `rgba(${rgb}, 0)`);

    context.fillStyle = gradient;
    context.fill();
}

// Draws a dual-line chart (specifically for Air Temp & Humidity)
function drawDualChart(context, cvs, dataArr1, dataArr2) {
    if(!cvs || !cvs.width || !context) return;
    const w = cvs.width / window.devicePixelRatio;
    const h = cvs.height / window.devicePixelRatio;

    context.clearRect(0, 0, w, h);

    // Draw Data Array 1 (Humidity - Primary Color #afc6ff)
    context.beginPath();
    context.strokeStyle = '#afc6ff';
    context.lineWidth = 3;
    const step = w / Math.max(dataArr1.length - 1, 1); // see drawChart() for why Math.max guards against a single-point buffer

    dataArr1.forEach((val, i) => {
        const x = i * step;
        const y = h - ((val / 100) * h * 0.8) - 20; // Scaled to 0-100%
        if(i === 0) context.moveTo(x, y);
        else context.lineTo(x, y);
    });
    context.stroke();

    // Draw Data Array 2 (Temp - Secondary Color #4edea3)
    context.beginPath();
    context.strokeStyle = '#4edea3';
    context.lineWidth = 3;

    dataArr2.forEach((val, i) => {
        const x = i * step;
        const y = h - ((val / 50) * h * 0.8) - 20; // Scaled to 0-50°C
        if(i === 0) context.moveTo(x, y);
        else context.lineTo(x, y);
    });
    context.stroke();
}

// Exports a given array of data to a CSV file (Offline capability)
function exportSeriesToCsv(sensorName, dataArr) {
    if (!dataArr || dataArr.length === 0) {
        alert("No data available to export.");
        return;
    }

    let csvContent = "data:text/csv;charset=utf-8,";
    csvContent += "Reading Index,Value\n"; // Header

    dataArr.forEach((val, index) => {
        // Because we don't have NTP time yet, we use a simple reading index
        csvContent += `${index},${val.toFixed(2)}\n`;
    });

    // Create a hidden link to trigger the download
    const encodedUri = encodeURI(csvContent);
    const link = document.createElement("a");
    link.setAttribute("href", encodedUri);

    // Timestamp for filename
    const date = new Date();
    const dateString = `${date.getFullYear()}${(date.getMonth()+1).toString().padStart(2, '0')}${date.getDate().toString().padStart(2, '0')}`;

    link.setAttribute("download", `hygrow_${sensorName}_${dateString}.csv`);
    document.body.appendChild(link);
    link.click();
    document.body.removeChild(link);
}
