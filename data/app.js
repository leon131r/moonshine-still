/**
 * Moonshine Controller - Main Application
 */

const App = {
    ws: null,
    config: null,
    snapshot: null,
    temps: [],
    maxTemps: 120,
    uptime: 0,
    
    // Initialize
    init() {
        console.log('[App] init v5');
        this.bindTabs();
        this.connectWS();
        this.loadConfig();
        this.bindControls();
        this.startUptime();
    },
    
    // Tab switching
    bindTabs() {
        document.querySelectorAll('.tabs > .tab').forEach(tab => {
            tab.onclick = () => {
                document.querySelectorAll('.tabs > .tab').forEach(t => t.classList.remove('active'));
                document.querySelectorAll('.tab-content').forEach(c => c.classList.remove('active'));
                tab.classList.add('active');
                document.getElementById('tab-' + tab.dataset.tab).classList.add('active');
            };
        });
        
        // Sub-tabs for settings
        document.querySelectorAll('#tab-settings .sub-tabs > .tab').forEach(tab => {
            tab.onclick = () => {
                document.querySelectorAll('#tab-settings .sub-tabs > .tab').forEach(t => t.classList.remove('active'));
                document.querySelectorAll('#tab-settings .sub-content').forEach(c => c.classList.remove('active'));
                tab.classList.add('active');
                document.getElementById('sub-' + tab.dataset.sub).classList.add('active');
            };
        });
    },
    
    // WebSocket
    connectWS() {
        const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
        this.ws = new WebSocket(proto + '//' + location.host + '/ws');
        
        this.ws.onopen = () => {
            document.getElementById('wsDot').classList.add('on');
            document.getElementById('wsText').textContent = 'WS';
            console.log('[App] WS connected');
        };
        
        this.ws.onclose = () => {
            document.getElementById('wsDot').classList.remove('on');
            document.getElementById('wsText').textContent = '—';
            setTimeout(() => this.connectWS(), 3000);
        };
        
        this.ws.onerror = (e) => console.error('[App] WS error', e);
        
        this.ws.onmessage = (e) => {
            try {
                const d = JSON.parse(e.data);
                console.log('[App] WS recv:', d);
                if (d.success !== undefined) {
                    this.toast(d.message || (d.success ? 'OK' : 'Error'), d.success ? 'ok' : 'err');
                }
                if (d.type === 'telemetry') this.onTelemetry(d);
                if (d.type === 'sensors' && d.sensors) {
                    this.updateSensorsTable(d.sensors);
                }
            } catch (x) {
                console.warn('[App] WS parse error:', x, 'raw:', e.data);
            }
        };
    },
    
    send(cmd, data = {}) {
        const msg = { cmd, ...data };
        if (this.ws && this.ws.readyState === 1) {
            this.ws.send(JSON.stringify(msg));
            console.log('[App] WS send:', cmd, data);
        } else {
            console.warn('[App] WS not connected');
            this.toast('WS не подключён', 'err');
        }
    },
    
    // Load config
    async loadConfig() {
        try {
            const r = await fetch('/api/config');
            this.config = await r.json();
            this.fillConfig();
        } catch (e) {
            console.error('[App] Config load failed', e);
        }
    },
    
    fillConfig() {
        if (!this.config) return;
        const c = this.config;
        const el = (id) => document.getElementById(id);
        
        // PID Boiler
        if (c.pid_cube) {
            if (el('pidBoilerKp')) el('pidBoilerKp').value = c.pid_cube.kp || 2.0;
            if (el('pidBoilerKi')) el('pidBoilerKi').value = c.pid_cube.ki || 0.5;
            if (el('pidBoilerKd')) el('pidBoilerKd').value = c.pid_cube.kd || 1.0;
        }
        
        // PID Cooler
        if (c.pid_cooler) {
            if (el('pidCoolerKp')) el('pidCoolerKp').value = c.pid_cooler.kp || 3.0;
            if (el('pidCoolerKi')) el('pidCoolerKi').value = c.pid_cooler.ki || 0.3;
            if (el('pidCoolerKd')) el('pidCoolerKd').value = c.pid_cooler.kd || 1.5;
        }
        
        // Thresholds
        if (el('thresholdHeads')) el('thresholdHeads').value = c.threshold_heads_end || 1.2;
        if (el('thresholdBody')) el('thresholdBody').value = c.threshold_body_end || 0.3;
        
        // Temperature setpoints
        if (el('setpointBoiler')) el('setpointBoiler').value = c.boiler_setpoint || 78.0;
        if (el('setpointCooler')) el('setpointCooler').value = c.cooler_setpoint || 35.0;
        if (el('distTargetTemp')) el('distTargetTemp').value = c.dist_target_temp || 78.0;
        if (el('warningPct')) el('warningPct').value = c.warning_percent || 90;
        
        // WiFi
        if (el('wifiSsid')) el('wifiSsid').value = c.wifi_ssid || '';
        const wifiSsid = el('wifiSsid');
        const wifiResults = el('wifiResults');
        if (wifiSsid && !document.getElementById('btnScanWifi')) {
            const scanBtn = document.createElement('button');
            scanBtn.id = 'btnScanWifi';
            scanBtn.className = 'btn btn-sm';
            scanBtn.textContent = '🔍';
            scanBtn.onclick = async () => {
                scanBtn.textContent = '...';
                try {
                    const r = await fetch('/api/wifi/scan');
                    const d = await r.json();
                    if (d.networks && wifiResults) {
                        wifiResults.innerHTML = '';
                        d.networks.forEach(n => {
                            const div = document.createElement('div');
                            div.style = 'padding:4px;cursor:pointer';
                            div.textContent = n.ssid + (n.rssi ? ' (' + n.rssi + ')' : '');
                            div.onclick = () => wifiSsid.value = n.ssid;
                            wifiResults.appendChild(div);
                        });
                    }
                } catch(e) {}
                scanBtn.textContent = '🔍';
            };
            wifiSsid.parentNode.appendChild(scanBtn);
        }
        
        // MQTT
        if (c.mqtt) {
            if (el('mqttEnabled')) el('mqttEnabled').checked = c.mqtt.enabled || false;
            if (el('mqttHost')) el('mqttHost').value = c.mqtt.host || '';
            if (el('mqttPort')) el('mqttPort').value = c.mqtt.port || 1883;
            if (el('mqttUser')) el('mqttUser').value = c.mqtt.user || '';
            if (el('mqttPass')) el('mqttPass').value = c.mqtt.pass || '';
            if (el('mqttTopic')) el('mqttTopic').value = c.mqtt.topic_root || c.mqtt.topic || 'distill';
        }
        
        // Sensors - with full editing
        if (c.sensors && c.sensors.length) {
            const tbody = document.querySelector('#sensorsTable tbody');
            if (tbody) {
                tbody.innerHTML = '';
                c.sensors.forEach((s, idx) => {
                    const tr = document.createElement('tr');
                    tr.dataset.idx = idx;
                    const tempVal = s.temp && s.temp > 0 ? s.temp.toFixed(2) : '--';
                    const offsetVal = s.offset || s.manual_offset || 0;
                    tr.innerHTML = `<td style="font-family:monospace;font-size:11px">${s.address || '—'}</td>
                        <td><select class="role-select" data-idx="${idx}">
                            <option value="column_top" ${s.role === 'column_top' ? 'selected' : ''}>column_top</option>
                            <option value="head_selection" ${s.role === 'head_selection' ? 'selected' : ''}>head_selection</option>
                            <option value="body_selection" ${s.role === 'body_selection' ? 'selected' : ''}>body_selection</option>
                            <option value="cooler" ${s.role === 'cooler' ? 'selected' : ''}>cooler</option>
                            <option value="boiler" ${s.role === 'boiler' ? 'selected' : ''}>boiler</option>
                        </select></td>
                        <td><input type="text" class="name-input" data-idx="${idx}" value="${s.name || ''}" style="width:80px;font-size:11px"></td>
                        <td>${tempVal}°C</td>
                        <td><input type="number" class="offset-input" data-idx="${idx}" value="${offsetVal}" step="0.01" style="width:50px;font-size:11px"></td>
                        <td><input type="checkbox" class="calib-check" data-idx="${idx}" ${s.calibrate ? 'checked' : ''}></td>
                        <td><input type="checkbox" class="active-check" data-idx="${idx}" ${s.active !== false ? 'checked' : ''}></td>
                        <td><button class="btn btn-xs save-sensor" data-idx="${idx}">💾</button></td>`;
                    tbody.appendChild(tr);
                });
                
                // Bind save buttons
                document.querySelectorAll('.save-sensor').forEach(btn => {
                    btn.onclick = () => this.saveSensor(parseInt(btn.dataset.idx));
                });
            }
        }
},

    // Save single sensor
    saveSensor(idx) {
        const row = document.querySelector(`#sensorsTable tbody tr[data-idx="${idx}"]`);
        if (!row) return;
        
        const sensor = {
            idx: idx,
            role: row.querySelector('.role-select').value,
            name: row.querySelector('.name-input').value,
            offset: parseFloat(row.querySelector('.offset-input').value) || 0,
            calibrate: row.querySelector('.calib-check').checked,
            active: row.querySelector('.active-check').checked
        };
        
        console.log('[App] Saving sensor:', sensor);
        this.send('set_sensor', sensor);
    },
    
    // Update sensors table from scan
    updateSensorsTable(sensors) {
        const tbody = document.querySelector('#sensorsTable tbody');
        if (!tbody) return;
        
        tbody.innerHTML = '';
        sensors.forEach((s, idx) => {
            const tr = document.createElement('tr');
            tr.dataset.idx = idx;
            const tempVal = s.temp && s.temp > 0 ? s.temp.toFixed(2) : '--';
            const offsetVal = s.offset || s.manual_offset || 0;
            tr.innerHTML = `<td style="font-family:monospace;font-size:11px">${s.address || '—'}</td>
                <td><select class="role-select" data-idx="${idx}">
                    <option value="column_top" ${s.role === 'column_top' ? 'selected' : ''}>column_top</option>
                    <option value="head_selection" ${s.role === 'head_selection' ? 'selected' : ''}>head_selection</option>
                    <option value="body_selection" ${s.role === 'body_selection' ? 'selected' : ''}>body_selection</option>
                    <option value="cooler" ${s.role === 'cooler' ? 'selected' : ''}>cooler</option>
                    <option value="boiler" ${s.role === 'boiler' ? 'selected' : ''}>boiler</option>
                </select></td>
                <td><input type="text" class="name-input" data-idx="${idx}" value="${s.name || ''}" style="width:80px;font-size:11px"></td>
                <td>${tempVal}°C</td>
                <td><input type="number" class="offset-input" data-idx="${idx}" value="${offsetVal}" step="0.01" style="width:50px;font-size:11px"></td>
                <td><input type="checkbox" class="calib-check" data-idx="${idx}" ${s.calibrate ? 'checked' : ''}></td>
                <td><input type="checkbox" class="active-check" data-idx="${idx}" ${s.active !== false ? 'checked' : ''}></td>
                <td><button class="btn btn-xs save-sensor" data-idx="${idx}">💾</button></td>`;
            tbody.appendChild(tr);
        });
        
        // Bind save button handlers
        document.querySelectorAll('.save-sensor').forEach(btn => {
            btn.onclick = () => this.saveSensor(parseInt(btn.dataset.idx));
        });
        
        this.toast(`Найдено датчиков: ${sensors.length}`, 'ok');
    },
    
    // Telemetry update
    onTelemetry(d) {
        this.snapshot = d;
        
        // Mode & Phase
        const modeBadge = document.getElementById('modeBadge');
        modeBadge.textContent = d.mode === 'distillation' ? 'ДИСТИЛЛЯЦИЯ' : 'РЕКТИФИКАЦИЯ';
        modeBadge.classList.toggle('dist', d.mode === 'distillation');
        
        const phaseBadge = document.getElementById('phaseBadge');
        const phaseNames = { idle: 'ОЖИДАНИЕ', heating: 'НАГРЕВ', heads: 'ГОЛОВЫ', body: 'ТЕЛО', tails: 'ХВОСТЫ', finish: 'ГОТОВО', error: 'АВАРИЯ' };
        phaseBadge.textContent = phaseNames[d.phase] || d.phase || '—';
        phaseBadge.style.background = d.phase === 'error' ? 'var(--danger)' : 
                                   d.phase === 'finish' ? 'var(--accent)' :
                                   d.phase === 'heads' ? 'var(--heads)' : 'var(--info)';
        
        // Temperatures (2 decimal places, fixed)
        const fmtTemp = (v) => v && v > 0 ? v.toFixed(2) + '°C' : '--.-°C';
        const fmtDelta = (v) => v && v !== 0 ? v.toFixed(2) : '--';
        document.getElementById('tempTop').textContent = fmtTemp(d.T_column_top);
        document.getElementById('tempSelect').textContent = fmtTemp(d.T_select);
        document.getElementById('deltaT').textContent = fmtDelta(d.delta_T);
        document.getElementById('tempBoiler').textContent = fmtTemp(d.T_boiler);
        document.getElementById('tempCooler').textContent = fmtTemp(d.T_cooler);
        
        // Mode switch
        const modeRadios = document.querySelectorAll('input[name="mode"]');
        modeRadios.forEach(r => {
            r.checked = (r.value === d.mode);
        });
        
        // Update container select if changed
        if (d.tank && d.tank.selected_id !== undefined) {
            const sel = document.getElementById('selContainer');
            if (sel && sel.value != d.tank.selected_id && sel.options) {
                for (let i = 0; i < sel.options.length; i++) {
                    if (parseInt(sel.options[i].value) == d.tank.selected_id) {
                        sel.selectedIndex = i;
                        break;
                    }
                }
            }
        }
        
        // Collection stopped state
        const tank = d.tank || {};
        const btnStopCollect = document.getElementById('btnStopCollect');
        const btnResumeCollect = document.getElementById('btnResumeCollect');
        const btnResetCnt = document.getElementById('btnResetCnt');
        if (btnStopCollect) btnStopCollect.style.display = tank.stopped ? 'none' : 'inline-block';
        if (btnResumeCollect) btnResumeCollect.style.display = tank.stopped ? 'inline-block' : 'none';
        if (btnResetCnt) btnResetCnt.style.display = 'inline-block';
        
        // Heater
        const heaterPct = d.heater_power || 0;
        document.getElementById('heaterBar').style.width = heaterPct + '%';
        document.getElementById('heaterPower').textContent = heaterPct + '%';
        
        // Cooler
        const coolerPct = d.cooler_power || 0;
        document.getElementById('coolerBar').style.width = coolerPct + '%';
        document.getElementById('coolerPower').textContent = coolerPct + '%';
        
        // Volume (Collection Tank)
        const vol = tank.current_ml || d.volume_ml || 0;
        const cap = tank.capacity_ml || tank.container_volume_ml || d.capacity_ml || 1000;
        const volPct = cap > 0 ? (vol * 100 / cap) : 0;
        document.getElementById('volumeBar').style.width = volPct + '%';
        document.getElementById('volumeMl').textContent = vol;
        document.getElementById('capacityMl').textContent = cap;
        document.getElementById('volumePct').textContent = volPct.toFixed(0);
        document.getElementById('sessionMl').textContent = tank.session_ml || '--';
        document.getElementById('abv').textContent = tank.abv || d.abv || '--';
        document.getElementById('rate').textContent = tank.fill_rate || '--';

// Container name in header
        const containerNameEl = document.getElementById('containerName');
        if (containerNameEl) {
            containerNameEl.textContent = tank.container_name || '';
        }
        
        // Calibration status
        if (d.calibrating) {
            document.getElementById('calibProgress').style.display = 'block';
            document.getElementById('calibPercent').textContent = d.calib_progress || 0;
            // Show temps during calibration
            if (d.temps) {
                const tempsStr = d.temps.map(t => `${t.role}:${t.temp?.toFixed(2)}°`).join(' ');
                document.getElementById('calibTemps').textContent = tempsStr;
            }
        } else {
            document.getElementById('calibProgress').style.display = 'none';
        }
    },
    
    // Controls
    bindControls() {
        // Main buttons
        document.getElementById('btnStart').onclick = () => this.send('start');
        document.getElementById('btnStop').onclick = () => this.send('stop');
        document.getElementById('btnEmergency').onclick = () => { if (confirm('Экстренная остановка?')) this.send('emergency_stop'); };
        document.getElementById('btnNextPhase').onclick = () => this.send('next_phase');
        
        // Error reset button (in header or dashboard)
        const btnResetError = document.getElementById('btnResetError');
        if (btnResetError) btnResetError.onclick = () => { if (confirm('Сбросить аварию?')) this.send('reset_error'); };
        
        // Continue collection
        document.getElementById('btnContinue').onclick = async () => {
            try {
                const r = await fetch('/api/tank/continue', { method: 'POST' });
                const d = await r.json();
                this.toast(d.success ? 'Продолжено' : 'Ошибка', d.success ? 'ok' : 'err');
            } catch(e) { this.toast('Ошибка сети', 'err'); }
        };
        
        // Stop collection (manual)
        document.getElementById('btnStopCollect').onclick = async () => {
            try {
                const r = await fetch('/api/tank/stop', { method: 'POST' });
                const d = await r.json();
                this.toast(d.success ? 'Отбор остановлен' : 'Ошибка', d.success ? 'ok' : 'err');
            } catch(e) { this.toast('Ошибка сети', 'err'); }
        };
        
        // Resume collection
        document.getElementById('btnResumeCollect').onclick = async () => {
            try {
                const r = await fetch('/api/tank/resume', { method: 'POST' });
                const d = await r.json();
                this.toast(d.success ? 'Отбор возобновлён' : 'Ошибка', d.success ? 'ok' : 'err');
            } catch(e) { this.toast('Ошибка сети', 'err'); }
        };
        
        // Container select
        this.loadContainerSelect();
        
        // Mode switch
        document.querySelectorAll('input[name="mode"]').forEach(r => {
            r.onchange = async () => {
                try {
                    const r2 = await fetch('/api/mode/set', {
                        method: 'POST',
                        headers: {'Content-Type': 'application/json'},
                        body: JSON.stringify({mode: r.value})
                    });
                    const d = await r2.json();
                    this.toast(d.message || 'Режим сохранён', d.success ? 'ok' : 'err');
                } catch(e) { this.toast('Ошибка сети', 'err'); }
            };
        });
        
        // Container controls
        document.getElementById('btnAddContainer').onclick = () => this.addContainer();
        this.loadContainers();
        
        // WiFi save button
        const btnSaveWifi = document.getElementById('btnSaveWifi');
        if (btnSaveWifi) {
            btnSaveWifi.onclick = async () => {
                const ssid = document.getElementById('wifiSsid').value;
                const pass = document.getElementById('wifiPass').value;
                if (!ssid) { this.toast('Введите SSID', 'err'); return; }
                try {
                    const r = await fetch('/api/wifi/save', {
                        method: 'POST',
                        headers: {'Content-Type': 'application/json'},
                        body: JSON.stringify({ssid, password: pass})
                    });
                    const d = await r.json();
                    this.toast(d.message || (d.success ? 'Сохранено' : 'Ошибка'), d.success ? 'ok' : 'err');
                } catch(e) { this.toast('Ошибка сети', 'err'); }
            };
        }
        
        // Save settings
        document.getElementById('btnSaveAll').onclick = () => this.saveAllSettings();
        document.getElementById('btnReboot').onclick = () => { if (confirm('Перезагрузить?')) { this.send('reboot'); } };
        document.getElementById('btnResetDefault').onclick = () => { if (confirm('Сбросить настройки?')) this.send('reset_default'); };
        
        // Scan sensors
        document.getElementById('btnScanSensors')?.addEventListener('click', () => { this.send('scan_sensors'); this.toast('Сканирование...', 'ok'); });
        
        // Calibrate button
        document.getElementById('btnCalibrate')?.addEventListener('click', () => { 
            this.send('calibrate'); 
            this.toast('Калибровка запущена...', 'ok');
            document.getElementById('calibProgress').style.display = 'block';
        });
        
        // Chart buttons
        document.getElementById('btnChartTemps').onclick = () => this.drawChart('temps');
        document.getElementById('btnChartPid').onclick = () => this.drawChart('pid');
        document.getElementById('btnChartVolume').onclick = () => this.drawChart('volume');
    },
    
    // Save all settings
    async saveAllSettings() {
        const el = (id, fallback = '') => {
            const e = document.getElementById(id);
            return e ? e.value : fallback;
        };
        const elc = (id) => {
            const e = document.getElementById(id);
            return e ? e.checked : false;
        };
        const settings = {
            pid_boiler: {
                kp: parseFloat(document.getElementById('pidBoilerKp')?.value || 2.0),
                ki: parseFloat(document.getElementById('pidBoilerKi')?.value || 0.5),
                kd: parseFloat(document.getElementById('pidBoilerKd')?.value || 1.0)
            },
            pid_cooler: {
                kp: parseFloat(document.getElementById('pidCoolerKp')?.value || 3.0),
                ki: parseFloat(document.getElementById('pidCoolerKi')?.value || 0.3),
                kd: parseFloat(document.getElementById('pidCoolerKd')?.value || 1.5)
            },
            threshold_heads_end: parseFloat(document.getElementById('thresholdHeads')?.value || 1.2),
            threshold_body_end: parseFloat(document.getElementById('thresholdBody')?.value || 0.3),
            mqtt: {
                enabled: elc('mqttEnabled'),
                host: el('mqttHost'),
                port: parseInt(el('mqttPort', '1883')),
                user: el('mqttUser'),
                pass: el('mqttPass'),
                topic: el('mqttTopic', 'distill')
            },
            boiler_setpoint: parseFloat(el('setpointBoiler', '78.0')),
            cooler_setpoint: parseFloat(el('setpointCooler', '35.0')),
            dist_target_temp: parseFloat(el('distTargetTemp', '78.0')),
            warning_percent: parseInt(el('warningPct', '90'))
        };
        
        try {
            const r = await fetch('/api/settings/save', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(settings)
            });
            const d = await r.json();
            this.toast(d.message || 'Сохранено', d.success ? 'ok' : 'err');
        } catch (e) {
            this.toast('Ошибка сохранения', 'err');
        }
    },
    
    // Uptime
    startUptime() {
        setInterval(() => {
            if (this.snapshot && this.snapshot.uptime) {
                const s = this.snapshot.uptime;
                const h = Math.floor(s / 3600);
                const m = Math.floor((s % 3600) / 60);
                const sec = s % 60;
                document.getElementById('uptime').textContent = 
                    String(h).padStart(2, '0') + ':' +
                    String(m).padStart(2, '0') + ':' +
                    String(sec).padStart(2, '0');
            }
        }, 1000);
    },
    
    // Draw chart
    drawChart(type = 'temps') {
        const canvas = document.getElementById('chartCanvas');
        const ctx = canvas.getContext('2d');
        const w = canvas.width = canvas.offsetWidth;
        const h = canvas.height = 300;
        
        ctx.fillStyle = '#0d1117';
        ctx.fillRect(0, 0, w, h);
        
        if (this.temps.length < 2) {
            ctx.fillStyle = '#8b949e';
            ctx.font = '14px sans-serif';
            ctx.textAlign = 'center';
            ctx.fillText('Нет данных', w/2, h/2);
            return;
        }
        
        // Find min/max
        let min = Infinity, max = -Infinity;
        this.temps.forEach(t => {
            const v = type === 'temps' ? t.top : (type === 'pid' ? t.cooler : 0);
            if (v < min) min = v;
            if (v > max) max = v;
        });
        if (max - min < 5) { min -= 2; max += 2; }
        
        const step = w / (this.temps.length - 1);
        
        // Draw line
        ctx.beginPath();
        ctx.strokeStyle = type === 'temps' ? '#58a6ff' : '#238636';
        ctx.lineWidth = 2;
        
        this.temps.forEach((t, i) => {
            const v = type === 'temps' ? t.top : (type === 'pid' ? t.cooler : 0);
            const y = h - ((v - min) / (max - min)) * (h - 40) - 20;
            const x = i * step;
            if (i === 0) ctx.moveTo(x, y);
            else ctx.lineTo(x, y);
        });
        ctx.stroke();
    },
    
    // Load containers
    async loadContainers() {
        try {
            const r = await fetch('/api/containers');
            const d = await r.json();
            const tbody = document.querySelector('#containersTable tbody');
            if (!tbody) return;
            tbody.innerHTML = '';
            (d.containers || []).forEach(c => {
                const tr = document.createElement('tr');
                tr.innerHTML = `<td>${c.name}</td>
                    <td>${c.volume_ml}</td>
                    <td>${c.diameter_mm}</td>
                    <td>
                        <button class="btn btn-sm" onclick="App.selectContainer(${c.id})">${c.id == d.selected_id ? '✓' : 'Выбрать'}</button>
                        <button class="btn btn-sm btn-danger" onclick="App.deleteContainer(${c.id})">✕</button>
                    </td>`;
                tbody.appendChild(tr);
            });
        } catch(e) { console.error('[App] Containers load failed', e); }
    },
    
    // Load container dropdown
    async loadContainerSelect() {
        try {
            const r = await fetch('/api/containers');
            const d = await r.json();
            const sel = document.getElementById('selContainer');
            if (!sel) return;
            sel.innerHTML = '<option value="">— Выберите ёмкость —</option>';
            (d.containers || []).forEach(c => {
                const opt = document.createElement('option');
                opt.value = c.id;
                opt.textContent = `${c.name} (${c.volume_ml}мл)`;
                if (c.id == d.selected_id) opt.selected = true;
                sel.appendChild(opt);
            });
            sel.onchange = () => {
                if (sel.value) this.selectContainer(parseInt(sel.value));
            };
        } catch(e) { console.error('[App] Container select load failed', e); }
    },
    
    // Select container
    async selectContainer(id) {
        try {
            const r = await fetch(`/api/container/select?id=${id}`, { method: 'POST' });
            const d = await r.json();
            if (d.success) {
                this.loadContainers();
                this.loadContainerSelect();
            }
            this.toast(d.success ? 'Ёмкость выбрана' : 'Ошибка', d.success ? 'ok' : 'err');
        } catch(e) { this.toast('Ошибка сети', 'err'); }
    },
    
    // Add container
    async addContainer() {
        const name = document.getElementById('newContainerName').value.trim();
        const volume = parseInt(document.getElementById('newContainerVolume').value);
        const diameter = parseInt(document.getElementById('newContainerDiameter').value);
        if (!name || !volume) { this.toast('Введите название и объём', 'err'); return; }
        try {
            const r = await fetch('/api/containers', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({name, volume_ml: volume, diameter_mm: diameter || 0})
            });
            const d = await r.json();
            if (d.success) {
                document.getElementById('newContainerName').value = '';
                document.getElementById('newContainerVolume').value = '';
                document.getElementById('newContainerDiameter').value = '';
                this.loadContainers();
            }
            this.toast(d.success ? 'Добавлено' : 'Ошибка', d.success ? 'ok' : 'err');
        } catch(e) { this.toast('Ошибка сети', 'err'); }
    },
    
    // Delete container
    async deleteContainer(id) {
        if (!confirm('Удалить ёмкость?')) return;
        try {
            const r = await fetch(`/api/containers/delete?id=${id}`, { method: 'DELETE' });
            const d = await r.json();
            if (d.success) this.loadContainers();
            this.toast(d.success ? 'Удалено' : 'Ошибка', d.success ? 'ok' : 'err');
        } catch(e) { this.toast('Ошибка сети', 'err'); }
    },
    
    // Toast notifications
    toast(msg, type = 'info') {
        const div = document.createElement('div');
        div.className = 'toast ' + type;
        div.textContent = msg;
        document.getElementById('toasts').appendChild(div);
        setTimeout(() => div.remove(), 3000);
    }
};

// Start
document.addEventListener('DOMContentLoaded', () => App.init());