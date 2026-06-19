/* ─── Config ──────────────────────────────────────── */
const POLL_INTERVAL = 5000; // ms

/* ─── State ───────────────────────────────────────── */
let pollTimer   = null;
let ringTimer   = null;
let ringStart   = null;
let serverDataMap = {}; // Maps server name to full metadata

/* ─── Countdown ring ──────────────────────────────── */
function startRing() {
    cancelAnimationFrame(ringTimer);
    ringStart = performance.now();
    const ring = document.getElementById('ring-fill');
    if (!ring) return;

    function tick(now) {
        const elapsed  = now - ringStart;
        const progress = Math.min(elapsed / POLL_INTERVAL, 1);
        ring.style.setProperty('--progress', progress);
        ring.style.strokeDashoffset = 100.53 * progress;
        if (progress < 1) ringTimer = requestAnimationFrame(tick);
    }
    ringTimer = requestAnimationFrame(tick);
}

/* ─── Spinner flash on the icon ───────────────────── */
function flashSpin() {
    const btn = document.getElementById('refresh-btn');
    if (!btn) return;
    btn.classList.remove('spinning');
    void btn.offsetWidth; // force reflow
    btn.classList.add('spinning');
    btn.addEventListener('animationend', () => btn.classList.remove('spinning'), { once: true });
}

/* ─── Update "last updated" timestamp ────────────── */
function setLastUpdated() {
    const el = document.getElementById('last-updated');
    if (!el) return;
    const now = new Date();
    el.textContent = `Updated ${now.toLocaleTimeString()}`;
}

/* ─── Render helpers ──────────────────────────────── */
function statusClass(status) {
    if (['active', 'up', 'online', 'healthy'].includes(status))       return 'active';
    if (['unreachable', 'down', 'offline', 'unhealthy'].includes(status)) return 'unreachable';
    return 'unknown';
}

function numCell(val) {
    return val != null
        ? `<span class="num">${val}</span>`
        : `<span class="num dim">—</span>`;
}

/* ─── Fetch & render ──────────────────────────────── */
async function toggleRoute(serverName, type, isEnabled) {
    try {
        const payload = {};
        if (type === 'inbound') {
            payload.inbound_enabled = isEnabled;
        } else {
            payload.outbound_enabled = isEnabled;
        }

        const res = await fetch(`/toggle-container/${serverName}`, {
            method: 'PUT',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(payload)
        });

        if (!res.ok) throw new Error('Failed to toggle');
        // Instantly reflect in local cache, next poll will confirm anyway
        if (serverDataMap[serverName]) {
            serverDataMap[serverName][type + '_enabled'] = isEnabled;
        }
    } catch (err) {
        alert('Failed to update toggle: ' + err.message);
        manualRefresh(); // revert toggle visually
    }
}

async function fetchServers() {
    const tbody = document.getElementById('servers-body');

    flashSpin();
    startRing();

    try {
        const response = await fetch('/list-containers');
        const resData  = await response.json();
        const servers  = resData.data || [];

        servers.sort((a, b) => (a.name || '').localeCompare(b.name || ''));

        setLastUpdated();
        tbody.innerHTML = '';

        if (servers.length === 0) {
            tbody.innerHTML = '<tr class="empty-row"><td colspan="9">No servers registered</td></tr>';
            serverDataMap = {};
            return;
        }

        serverDataMap = {};

        servers.forEach(server => {
            serverDataMap[server.name] = server;
            const h      = server.health || {};
            const status = (h.status || 'unknown').toLowerCase();
            const cls    = statusClass(status);
            const label  = (h.status || 'Unknown').toUpperCase();

            const inboundChecked = server.inbound_enabled ? 'checked' : '';
            const outboundChecked = server.outbound_enabled ? 'checked' : '';

            const tr = document.createElement('tr');
            tr.onclick = (e) => {
                // Prevent opening panel if clicking the toggle
                const isToggle = e.target.closest('.switch');
                console.log("isToggle: ", isToggle);
                if (isToggle) return;  
                
                openPanel(server.name);
            };
            tr.innerHTML = `
                <td><span class="status ${cls}">${label}</span></td>
                <td>${server.name || '—'}</td>
                <td class="addr">${server.host}:${server.port}</td>
                <td>${numCell(h.max_calls)}</td>
                <td>${numCell(h.current_calls)}</td>
                <td>${numCell(h.available_capacity)}</td>
                <td>${numCell(h.answered_calls)}</td>
                <td>
                    <label class="switch" onclick="event.stopPropagation()">
                        <input type="checkbox" onchange="toggleRoute('${server.name}', 'inbound', this.checked)"" ${inboundChecked}>
                        <span class="slider round"></span>
                    </label>
                </td>
                <td>
                    <label class="switch" onclick="event.stopPropagation()">
                        <input type="checkbox" onchange="toggleRoute('${server.name}', 'outbound', this.checked)"" ${outboundChecked}>
                        <span class="slider round"></span>
                    </label>
                </td>
            `;
            tbody.appendChild(tr);
        });

    } catch (err) {
        tbody.innerHTML = `<tr class="error-row"><td colspan="9">⚠ ${err.message}</td></tr>`;
        setLastUpdated();
    }
}

/* ─── Manual refresh ──────────────────────────────── */
function manualRefresh() {
    clearTimeout(pollTimer);
    fetchServers().finally(schedulePoll);
}

/* ─── Polling loop ────────────────────────────────── */
function schedulePoll() {
    pollTimer = setTimeout(() => {
        fetchServers().finally(schedulePoll);
    }, POLL_INTERVAL);
}

/* ─── Boot ────────────────────────────────────────── */
fetchServers().finally(schedulePoll);
/* ─── Side Panel ──────────────────────────────────── */
function openPanel(serverName) {
    const server = serverDataMap[serverName];
    if (!server) return;

    document.getElementById('panel-title').textContent = server.name || 'Server Details';
    
    const h = server.health || {};
    const errorHtml = h.error 
        ? `<div class="meta-item"><span class="meta-label">Recent Error</span><span class="meta-value" style="color:var(--red); border-color:var(--red-bg);">${h.error}</span></div>` 
        : '';

    const html = `
        <div class="meta-group">
            <h3>Networking</h3>
            <div class="meta-item"><span class="meta-label">Host</span><span class="meta-value">${server.host}</span></div>
            <div class="meta-item"><span class="meta-label">Port</span><span class="meta-value">${server.port}</span></div>
        </div>
        <div class="meta-group">
            <h3>SIP Details</h3>
            <div class="meta-item"><span class="meta-label">AOR (Address of Record)</span><span class="meta-value">${server.aor || '-'}</span></div>
            <div class="meta-item"><span class="meta-label">Contact URI</span><span class="meta-value">${server.contact_uri || '-'}</span></div>
        </div>
        <div class="meta-group">
            <h3>Target Health & Capacity</h3>
            <div class="meta-item"><span class="meta-label">Status</span><span class="meta-value" style="text-transform: capitalize;">${h.status || 'Unknown'}</span></div>
            <div class="meta-item"><span class="meta-label">Max Calls</span><span class="meta-value">${h.max_calls ?? '-'}</span></div>
            <div class="meta-item"><span class="meta-label">Current Calls</span><span class="meta-value">${h.current_calls ?? '-'}</span></div>
            <div class="meta-item"><span class="meta-label">Available Capacity</span><span class="meta-value">${h.available_capacity ?? '-'}</span></div>
            <div class="meta-item"><span class="meta-label">Answered Calls</span><span class="meta-value">${h.answered_calls ?? '-'}</span></div>
            <div class="meta-item"><span class="meta-label">Last Checked</span><span class="meta-value">${h.timestamp ? new Date(h.timestamp * 1000).toLocaleString() : '-'}</span></div>
            ${errorHtml}
        </div>
    `;
    
    document.getElementById('panel-content').innerHTML = html;
    document.getElementById('overlay').classList.add('open');
    document.getElementById('panel').classList.add('open');
}

function closePanel() {
    document.getElementById('overlay').classList.remove('open');
    document.getElementById('panel').classList.remove('open');
}


// Close panel on escape key
document.addEventListener('keydown', (e) => {
    if (e.key === 'Escape') closePanel();
});

// Explicitly expose functions to global scope to prevent ReferenceErrors from inline HTML attributes
window.closePanel = closePanel;
window.manualRefresh = manualRefresh;
window.openPanel = openPanel;
window.toggleRoute = toggleRoute;

