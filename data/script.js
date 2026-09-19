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

