// 1. Three.js 3D Vehicle Setup
let scene, camera, renderer, carMesh, frontWheels = [];
function init3D() {
  const container = document.getElementById('car3d-container');
  scene = new THREE.Scene();
  camera = new THREE.PerspectiveCamera(45, container.clientWidth / container.clientHeight, 0.1, 100);
  camera.position.set(3, 2.5, 4);
  camera.lookAt(0, 0, 0);

  renderer = new THREE.WebGLRenderer({ antialias: true, alpha: true });
  renderer.setSize(container.clientWidth, container.clientHeight);
  container.appendChild(renderer.domElement);

  const light1 = new THREE.DirectionalLight(0x38bdf8, 1.5);
  light1.position.set(5, 10, 7);
  scene.add(light1);
  const light2 = new THREE.AmbientLight(0x1e293b, 1.0);
  scene.add(light2);

  // Car Body
  const bodyGeo = new THREE.BoxGeometry(2, 0.5, 1);
  const bodyMat = new THREE.MeshStandardMaterial({ color: 0x0284c7, metalness: 0.8, roughness: 0.2 });
  carMesh = new THREE.Mesh(bodyGeo, bodyMat);
  scene.add(carMesh);

  // Cabin
  const cabinGeo = new THREE.BoxGeometry(1, 0.4, 0.8);
  const cabinMat = new THREE.MeshStandardMaterial({ color: 0x0f172a, roughness: 0.1 });
  const cabin = new THREE.Mesh(cabinGeo, cabinMat);
  cabin.position.set(-0.2, 0.45, 0);
  carMesh.add(cabin);

  // Wheels
  const wheelGeo = new THREE.CylinderGeometry(0.2, 0.2, 0.15, 16);
  const wheelMat = new THREE.MeshStandardMaterial({ color: 0x334155, roughness: 0.9 });
  wheelGeo.rotateX(Math.PI / 2);

  const positions = [
    [0.7, -0.2, 0.55],  // Front Right
    [0.7, -0.2, -0.55], // Front Left
    [-0.7, -0.2, 0.55], // Rear Right
    [-0.7, -0.2, -0.55] // Rear Left
  ];

  positions.forEach((pos, idx) => {
    const wheel = new THREE.Mesh(wheelGeo, wheelMat);
    wheel.position.set(...pos);
    carMesh.add(wheel);
    if (idx < 2) frontWheels.push(wheel);
  });

  // Grid ground
  const grid = new THREE.GridHelper(20, 20, 0x38bdf8, 0x1e293b);
  grid.position.y = -0.4;
  scene.add(grid);

  function animate() {
    requestAnimationFrame(animate);
    grid.position.x = (grid.position.x - 0.05) % 1;
    renderer.render(scene, camera);
  }
  animate();
}
init3D();

// 2. Blackbox Replay State & Telemetry Polling Loop
let isReplayMode = false;
let replayIndex = -1;
let cachedHistory = [];
let lastViolCode = 0;

function setLiveMode() {
  isReplayMode = false;
  document.getElementById('btnLive').className = 'mode-btn active';
  document.getElementById('btnReplay').className = 'mode-btn';
  document.getElementById('renderModeLabel').innerText = 'LIVE TRACKING';
  document.getElementById('renderModeLabel').style.color = 'var(--text-muted)';
  const slider = document.getElementById('replaySlider');
  slider.value = cachedHistory.length ? cachedHistory.length - 1 : 299;
  document.getElementById('replayTimeLabel').innerText = '0.0s (Live Edge)';
}

function setReplayMode() {
  if (!cachedHistory.length) return;
  isReplayMode = true;
  document.getElementById('btnLive').className = 'mode-btn';
  document.getElementById('btnReplay').className = 'mode-btn replay-active';
  document.getElementById('renderModeLabel').innerText = 'REPLAY PAUSED';
  document.getElementById('renderModeLabel').style.color = 'var(--neon-amber)';
  if (replayIndex < 0) {
    replayIndex = cachedHistory.length - 1;
  }
  onScrubberInput(replayIndex);
}

function onScrubberInput(val) {
  if (!cachedHistory.length) return;
  isReplayMode = true;
  document.getElementById('btnLive').className = 'mode-btn';
  document.getElementById('btnReplay').className = 'mode-btn replay-active';
  document.getElementById('renderModeLabel').innerText = 'REPLAY SCRUB';
  document.getElementById('renderModeLabel').style.color = 'var(--neon-amber)';

  replayIndex = Math.min(Math.max(0, parseInt(val)), cachedHistory.length - 1);
  const frame = cachedHistory[replayIndex];
  const liveT = cachedHistory[cachedHistory.length - 1].t || 0;
  const frameT = frame.t || 0;
  const offsetSeconds = Math.max(0, liveT - frameT).toFixed(2);
  document.getElementById('replayTimeLabel').innerText = `-${offsetSeconds}s (Frame ${replayIndex}/${cachedHistory.length})`;

  renderFrame(frame);
}

function stepFrame(delta) {
  if (!cachedHistory.length) return;
  if (!isReplayMode) setReplayMode();
  const slider = document.getElementById('replaySlider');
  let target = (replayIndex >= 0 ? replayIndex : cachedHistory.length - 1) + delta;
  target = Math.min(Math.max(0, target), cachedHistory.length - 1);
  slider.value = target;
  onScrubberInput(target);
}

function renderFrame(frame) {
  if (!frame) return;
  document.getElementById('steerVal').innerText = (frame.steering || 0).toFixed(1) + '°';
  document.getElementById('speedVal').innerText = (frame.speed || 0).toFixed(0) + ' km/h';
  document.getElementById('throttleVal').innerText = (frame.throttle !== undefined ? frame.throttle : 0).toFixed(0) + ' %';

  const score = frame.score !== undefined ? frame.score : 100.0;
  document.getElementById('scoreVal').innerText = Math.round(score);
  const ring = document.getElementById('scoreRing');
  const offset = 264 - (264 * (score / 100));
  ring.style.strokeDashoffset = offset;

  const scoreLabel = document.getElementById('scoreLabel');
  if (score >= 90) {
    ring.style.stroke = 'var(--neon-green)';
    scoreLabel.innerText = 'SAFE';
    scoreLabel.style.color = 'var(--neon-green)';
  } else if (score >= 50) {
    ring.style.stroke = 'var(--neon-amber)';
    scoreLabel.innerText = 'WARNING';
    scoreLabel.style.color = 'var(--neon-amber)';
  } else {
    ring.style.stroke = 'var(--neon-red)';
    scoreLabel.innerText = 'CRITICAL';
    scoreLabel.style.color = 'var(--neon-red)';
  }

  const fsmBadge = document.getElementById('fsmBadge');
  if (fsmBadge && frame.fsm) {
    fsmBadge.innerText = `FSM (Est.): ${frame.fsm}`;
    fsmBadge.style.borderColor = frame.fsm_color || 'var(--neon-green)';
    fsmBadge.style.color = frame.fsm_color || 'var(--neon-green)';
    fsmBadge.style.background = `${frame.fsm_color || '#10b981'}25`;
  }

  const violEl = document.getElementById('violCode');
  if (frame.viol && frame.viol !== 'NONE') {
    violEl.innerText = frame.viol;
    violEl.style.color = 'var(--neon-red)';
  } else {
    violEl.innerText = '0x0 (NONE)';
    violEl.style.color = 'var(--neon-green)';
  }

  // Update 3D steering
  const steerRad = ((frame.steering || 0) * Math.PI) / 180;
  frontWheels.forEach(w => w.rotation.y = -steerRad * 0.4);
  carMesh.rotation.y = -steerRad * 0.15;
}

async function updateTelemetry() {
  try {
    const res = await fetch('/api/telemetry');
    const data = await res.json();

    cachedHistory = data.history || [];
    const slider = document.getElementById('replaySlider');
    slider.max = Math.max(0, cachedHistory.length - 1);

    // Connection badge
    const connText = document.getElementById('connText');
    const connDot = document.getElementById('connDot');
    const connStatus = document.getElementById('connStatus');

    if (data.status === 'CONNECTED') {
      connText.innerText = 'QNX 8.0 RTOS LIVE LINK';
      connDot.style.background = '#10b981';
      connStatus.style.borderColor = '#10b981';
    } else {
      connText.innerText = 'Waiting for QNX Node...';
      connDot.style.background = '#f59e0b';
      connStatus.style.borderColor = '#f59e0b';
    }

    document.getElementById('packetCount').innerText = (data.total_packets || 0).toLocaleString();
    const schedBadge = document.getElementById('schedBadge');
    if (schedBadge && data.sched_policy) {
      schedBadge.innerText = data.sched_policy;
    }

    // Incident Logging
    if (data.violation_code !== 0 && data.violation_code !== lastViolCode) {
      addLog(`[VIOLATION TRAP] ${data.violation_name} (Code: 0x${data.violation_code.toString(16).toUpperCase()})`, true);
      lastViolCode = data.violation_code;
    } else if (data.violation_code === 0) {
      lastViolCode = 0;
    }

    // In LIVE mode, render real-time state
    if (!isReplayMode) {
      slider.value = cachedHistory.length ? cachedHistory.length - 1 : 0;
      document.getElementById('velVal').innerText = (data.steering_vel || 0).toFixed(1) + ' °/s';
      renderFrame({
        steering: data.steering,
        speed: data.speed,
        throttle: data.throttle,
        score: data.safety_score,
        fsm: data.fsm_state,
        fsm_color: data.fsm_color,
        viol: data.violation_name
      });
    }

    // Always redraw Waveform with entire history
    drawWaveform(cachedHistory, isReplayMode ? replayIndex : -1);
  } catch (err) {
    console.error(err);
  }
}
setInterval(updateTelemetry, 100);

// 3. Canvas Waveform with Scrubber Marker
function drawWaveform(history, highlightIdx) {
  const canvas = document.getElementById('chart');
  const ctx = canvas.getContext('2d');
  const w = canvas.width;
  const h = canvas.height;
  ctx.clearRect(0, 0, w, h);

  if (!history.length) return;
  const maxPts = Math.max(history.length, 60);

  // Draw Grid
  ctx.strokeStyle = 'rgba(255, 255, 255, 0.05)';
  ctx.lineWidth = 1;
  for (let y = 30; y < h; y += 40) {
    ctx.beginPath();
    ctx.moveTo(0, y);
    ctx.lineTo(w, y);
    ctx.stroke();
  }

  // Draw Steering Line (Cyan)
  ctx.strokeStyle = '#38bdf8';
  ctx.lineWidth = 2;
  ctx.beginPath();
  history.forEach((pt, i) => {
    const x = (i / maxPts) * w;
    const y = h / 2 - ((pt.steering || 0) / 100) * (h / 2);
    if (i === 0) ctx.moveTo(x, y);
    else ctx.lineTo(x, y);
  });
  ctx.stroke();

  // Draw Speed Line (Emerald)
  ctx.strokeStyle = '#10b981';
  ctx.lineWidth = 1.5;
  ctx.beginPath();
  history.forEach((pt, i) => {
    const x = (i / maxPts) * w;
    const y = h - ((pt.speed || 0) / 120) * h;
    if (i === 0) ctx.moveTo(x, y);
    else ctx.lineTo(x, y);
  });
  ctx.stroke();

  // Draw Replay Scrubber Line (Amber)
  if (highlightIdx >= 0 && highlightIdx < history.length) {
    const scrubX = (highlightIdx / maxPts) * w;
    ctx.strokeStyle = '#f59e0b';
    ctx.lineWidth = 2;
    ctx.setLineDash([4, 4]);
    ctx.beginPath();
    ctx.moveTo(scrubX, 0);
    ctx.lineTo(scrubX, h);
    ctx.stroke();
    ctx.setLineDash([]);

    // Scrubber tag
    ctx.fillStyle = '#f59e0b';
    ctx.font = '10px monospace';
    ctx.fillText(`SCRUBBER [${highlightIdx}]`, Math.min(scrubX + 4, w - 90), 14);
  }
}

// 4. Send Chaos Command
async function sendChaos(type) {
  try {
    const res = await fetch(`/api/chaos/${type}`, { method: 'POST' });
    const resData = await res.json();
    addLog(`[CHAOS DISPATCH] Fault Type ${type}: ${resData.message}`, true);
  } catch (e) {
    addLog(`[ERROR] Chaos dispatch failed: ${e}`, true);
  }
}

function addLog(text, isViol = false) {
  const list = document.getElementById('logList');
  const item = document.createElement('div');
  item.className = 'log-item' + (isViol ? ' viol' : '');
  const timeStr = new Date().toLocaleTimeString();
  item.innerHTML = `<span>${text}</span><span style="color: var(--text-muted);">${timeStr}</span>`;
  list.prepend(item);
}
