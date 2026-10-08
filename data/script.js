// Functions to load pages
function loadWiFiManager() {
  fetch('/wifi-manager')
    .then(response => response.text())
    .then(html => {
      document.body.innerHTML = html;
      updateSSIDList(); // Call the function after updating the HTML
    });
}

var _statusPollInterval = null;

function loadStatus() {
  fetch('/status.html')
    .then(response => response.text())
    .then(html => {
      document.body.innerHTML = html;
      refreshStatus(); // call after updating the HTML, same as the other page loaders
      if (_statusPollInterval) clearInterval(_statusPollInterval);
      _statusPollInterval = setInterval(refreshStatus, 2000);
    });
}

function loadInfluxDbManager() {
  fetch('/influxdb-manager')
    .then(response => response.text())
    .then(html => {
      document.body.innerHTML = html;
      prefillInfluxDbForm();
    });
}

var _otaPollInterval = null;

function loadFirmwareUpdate() {
  fetch('/getFirmwareStatus')
    .then(r => r.json())
    .then(data => {
      var cv = document.getElementById('currentVersion');
      if (cv) cv.textContent = data.currentVersion || '?';
      _applyFirmwareStatus(data);
    })
    .catch(() => {});
}

function checkForUpdate() {
  document.getElementById('statusMsg').textContent = 'Checking…';
  document.getElementById('checkBtn').disabled = true;
  document.getElementById('updateBtn').style.display = 'none';
  fetch('/checkFirmwareUpdate', { method: 'POST' })
    .then(r => r.json())
    .then(() => { _startOtaPoll(); })
    .catch(e => {
      document.getElementById('statusMsg').textContent = 'Error: ' + e;
      document.getElementById('checkBtn').disabled = false;
    });
}

function performOtaUpdate() {
  document.getElementById('statusMsg').textContent = 'Installing update… device will restart.';
  document.getElementById('updateBtn').disabled = true;
  fetch('/performOTA', { method: 'POST' })
    .then(r => r.json())
    .then(() => { _startOtaPoll(); })
    .catch(e => {
      document.getElementById('statusMsg').textContent = 'Error: ' + e;
    });
}

function _startOtaPoll() {
  if (_otaPollInterval) clearInterval(_otaPollInterval);
  _otaPollInterval = setInterval(function () {
    fetch('/getFirmwareStatus')
      .then(r => r.json())
      .then(data => { _applyFirmwareStatus(data); })
      .catch(() => {});
  }, 2000);
}

function _applyFirmwareStatus(data) {
  var statusEl = document.getElementById('statusMsg');
  var latestEl = document.getElementById('latestVersion');
  var checkBtn = document.getElementById('checkBtn');
  var updateBtn = document.getElementById('updateBtn');
  if (!statusEl) return;

  if (latestEl && data.latestVersion) latestEl.textContent = data.latestVersion;

  switch (data.status) {
    case 'checking':
      statusEl.textContent = 'Checking for updates…';
      break;
    case 'up_to_date':
      statusEl.textContent = '✓ Firmware is up to date.';
      if (checkBtn) checkBtn.disabled = false;
      if (updateBtn) updateBtn.style.display = 'none';
      if (_otaPollInterval) { clearInterval(_otaPollInterval); _otaPollInterval = null; }
      break;
    case 'update_available':
      statusEl.textContent = 'Update available: ' + (data.latestVersion || data.latestTag);
      if (checkBtn) checkBtn.disabled = false;
      if (updateBtn) { updateBtn.style.display = ''; updateBtn.disabled = false; }
      if (_otaPollInterval) { clearInterval(_otaPollInterval); _otaPollInterval = null; }
      break;
    case 'updating':
      statusEl.textContent = 'Installing… do not power off.';
      break;
    case 'error':
      statusEl.textContent = '✗ Error. Check that the device is connected to the internet.';
      if (checkBtn) checkBtn.disabled = false;
      if (_otaPollInterval) { clearInterval(_otaPollInterval); _otaPollInterval = null; }
      break;
    default:
      break;
  }
}

// Function to update SSID list values dynamically
function updateSSIDList() {
  fetch('/getSSIDList')
    .then(response => response.json())
    .then(data => {
      for (var key in data) {
        if (data.hasOwnProperty(key)) {
          var option = document.getElementById(key);
          if (option) {
            option.value = data[key];
            option.textContent = data[key];
          }
        }
      }
    })
}

// Call the function on page load — only when on the WiFi manager page
window.onload = function () {
  if (document.getElementById('SSID1')) {
    updateSSIDList();
    setInterval(updateSSIDList, 15000); // Update every 15 seconds
  }
};

// Send WiFi manager form and poll for connection result
function sendWifiManagerForm() {
  const form = document.getElementById('wifiManagerForm');
  const formData = new FormData(form);

  // Replace form with a live status message
  const statusEl = document.createElement('p');
  statusEl.id = 'wifiStatus';
  statusEl.textContent = 'Connecting to WiFi…';
  form.replaceWith(statusEl);

  fetch('/wifi-manager', { method: 'POST', body: formData }).then(() => {
    let tries = 0;
    const timer = setInterval(() => {
      tries++;
      fetch('/getWifiStatus')
        .then(r => r.json())
        .then(data => {
          if (data.connected) {
            clearInterval(timer);
            document.getElementById('wifiStatus').innerHTML =
              '✓ Connected! Device IP: <strong>' + data.ip + '</strong>.<br>' +
              'Reconnect to your home network and navigate to ' +
              '<a href="http://' + data.ip + '">http://' + data.ip + '</a>.';
          } else if (tries >= 15) {
            clearInterval(timer);
            document.getElementById('wifiStatus').innerHTML =
              '✗ Connection failed. <a href="/wifi-manager">Try again</a>.';
          }
        })
        .catch(() => {
          // The AP went away — device is likely connected and stopped broadcasting
          clearInterval(timer);
          document.getElementById('wifiStatus').textContent =
            'Device has connected — reconnect to your home network.';
        });
    }, 2000);
  });
}

// Pre-fill InfluxDB form with current saved credentials
function prefillInfluxDbForm() {
  fetch('/getInfluxCredentials')
    .then(response => response.json())
    .then(data => {
      if (data.kilnName) document.getElementById('influxKilnName').value = data.kilnName;
      if (data.url)    document.getElementById('influxUrl').value    = data.url;
      if (data.token)  document.getElementById('influxToken').value  = data.token;
      if (data.org)    document.getElementById('influxOrg').value    = data.org;
      if (data.bucket) document.getElementById('influxBucket').value = data.bucket;
      if (data.tzInfo) {
        const select = document.getElementById('influxTzSelect');
        const matched = Array.from(select.options).some(opt => opt.value === data.tzInfo);
        if (matched) {
          select.value = data.tzInfo;
        } else {
          // Saved value doesn't match a preset (e.g. a hand-typed string from
          // before this dropdown existed) — surface it in the custom field
          // rather than silently dropping it.
          select.value = '__custom__';
          document.getElementById('influxTzCustom').value = data.tzInfo;
        }
        onTzSelectChange();
      }
    })
    .catch(() => {}); // no credentials saved yet — leave fields empty
}

// Show/hide the custom timezone field based on the dropdown selection
function onTzSelectChange() {
  const isCustom = document.getElementById('influxTzSelect').value === '__custom__';
  document.getElementById('influxTzCustom').style.display = isCustom ? '' : 'none';
}

// Send InfluxDB manager form
function sendInfluxDbForm() {
  const form = document.getElementById('influxDbForm');
  const formData = new FormData(form);

  if (document.getElementById('influxTzSelect').value === '__custom__') {
    formData.set('tzInfo', document.getElementById('influxTzCustom').value);
  }

  fetch('/influxdb-manager', {
    method: 'POST',
    body: formData
  })
    .then(response => response.text())
    .then(result => {
      console.log(result);
      window.location.href = '/index.html';
    })
}

// Converts a raw Celsius reading for display, matching gui.cpp's displayTemperature().
function displayTemperature(celsius, scale) {
  return scale === 'F' ? (celsius * 9.0 / 5.0 + 32.0) : celsius;
}

// Load and render this device's list of firing sessions (sessions.html)
function loadSessions() {
  const el = document.getElementById('sessionsList');
  if (!el) return;
  fetch('/getSessions')
    .then(response => {
      if (!response.ok) throw new Error('status ' + response.status);
      return response.json();
    })
    .then(data => {
      const sessions = data.sessions || [];
      if (sessions.length === 0) {
        el.innerHTML = '<p>No firing sessions recorded yet.</p>';
        return;
      }
      sessions.sort((a, b) => b.id - a.id); // newest first
      let html = '';
      sessions.forEach(s => {
        const start = s.startTime ? new Date(s.startTime).toLocaleString() : '?';
        const end = s.endTime ? new Date(s.endTime).toLocaleString() : '?';
        html += '<div class="program-card">' +
          '<div class="card-info">' +
          '<h3>Session ' + s.id + '</h3>' +
          '<small>' + start + ' &ndash; ' + end + ' &middot; ' + s.points + ' points</small>' +
          '</div>' +
          '<button class="button-segment" onclick="window.location.href=\'/downloadSession?id=' + s.id + '\'">Download CSV</button>' +
          '</div>';
      });
      el.innerHTML = html;
    })
    .catch(() => {
      el.innerHTML = '<p>Failed to load sessions. Is InfluxDB configured?</p>';
    });
}

// Orton pyrometric cone equivalents — Large Cones, Regular composition,
// 108°F/hr (60°C/hr) heating rate, in Celsius (matches the raw units
// /getStatus reports temperatures in). Source: Edward Orton Jr. Ceramic
// Foundation, "Temperature Equivalent Chart for Orton Pyrometric Cones",
// cone numbers 022-14 (ortonceramic.com) — table capped at cone 12 here,
// which covers the practical range for this kind of kiln; cones 13/14 use a
// different composition per Orton's own chart and aren't included. This is
// a peak-temperature approximation, not true heat-work — a cone's actual
// bending also depends on soak time and heating rate, so treat it as a
// rough guide alongside a real witness cone, not a replacement for one.
const ORTON_CONE_CHART_C = [
  ['019', 676], ['018', 712], ['017', 736], ['016', 769], ['015', 788],
  ['014', 807], ['013', 837], ['012', 858], ['011', 873], ['010', 898],
  ['09', 917], ['08', 942], ['07', 973], ['06', 995], ['05', 1030],
  ['04', 1060], ['03', 1086], ['02', 1101], ['01', 1117], ['1', 1136],
  ['2', 1142], ['3', 1152], ['4', 1160], ['5', 1184], ['6', 1220],
  ['7', 1237], ['8', 1247], ['9', 1257], ['10', 1282], ['11', 1293],
  ['12', 1304]
];

// Highest Orton cone reached at a given peak Celsius temperature — null
// below the chart's lowest cone (019), '12+' at or above its highest (12).
function celsiusToOrtonCone(celsius) {
  if (celsius < ORTON_CONE_CHART_C[0][1]) return null;
  let reached = ORTON_CONE_CHART_C[0][0];
  for (const [cone, temp] of ORTON_CONE_CHART_C) {
    if (celsius >= temp) reached = cone; else break;
  }
  const top = ORTON_CONE_CHART_C[ORTON_CONE_CHART_C.length - 1];
  return celsius >= top[1] ? top[0] + '+' : reached;
}

// Poll live zone status (used by status.html)
function refreshStatus() {
  fetch('/getStatus')
    .then(response => response.json())
    .then(data => {
      const el = document.getElementById('statusData');
      if (!el) return;
      const scale = data.tempScale || 'C';
      const unit = '&deg;' + scale;
      const badgeClass = data.loggingActive ? 'badge-active' : 'badge-waiting';
      let html = '<p><span class="badge ' + badgeClass + '">' + (data.loggingActive ? 'LOGGING ACTIVE' : 'WAITING') + '</span></p>';
      html += '<p>Ambient baseline: ' + displayTemperature(data.ambientBaseline, scale).toFixed(1) + unit + ' &mdash; TC type: ' + data.tcType + '</p>';
      if (typeof data.maxTemperature === 'number' && data.maxTemperature > -900) {
        const maxDisplay = displayTemperature(data.maxTemperature, scale).toFixed(1) + unit;
        const cone = celsiusToOrtonCone(data.maxTemperature);
        html += '<p>Max temperature: ' + maxDisplay + ' &mdash; ' + (cone ? ('~Cone ' + cone) : 'below cone 019') + '</p>';
      }
      html += '<ul class="zone-list">';
      data.zones.forEach((z, i) => {
        if (!z.active) return;
        const faultClass = z.fault ? ' class="fault"' : '';
        const status = z.fault ? '<span>FAULT</span>' : '';
        html += '<li' + faultClass + '><span>Zone ' + (i + 1) + '</span><span>' + displayTemperature(z.pv, scale).toFixed(1) + unit + '</span>' + status + '</li>';
      });
      html += '</ul>';
      el.innerHTML = html;
    })
    .catch(() => {});
}

