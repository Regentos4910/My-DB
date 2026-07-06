// app.js — click-not-type front end. Talks to the REST API only; no SQL
// is ever typed by the user (the optional Advanced SQL tab is the one
// deliberate exception, kept off to the side).

const content = document.getElementById('content');
const tableList = document.getElementById('table-list');
let schema = []; // cached list of table defs: {name, columns:[...]}
let activeTable = null;

function escapeHtml(s) {
  return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
}

async function api(path, opts) {
  const res = await fetch(path, opts);
  const text = await res.text();
  let body;
  try { body = text ? JSON.parse(text) : {}; } catch (e) { body = { raw: text }; }
  if (!res.ok || body.ok === false) {
    throw new Error(body.error || `request failed (${res.status})`);
  }
  return body;
}

function banner(kind, msg) {
  const div = document.createElement('div');
  div.className = kind === 'error' ? 'error-banner' : 'ok-banner';
  div.textContent = msg;
  content.prepend(div);
  setTimeout(() => div.remove(), 6000);
}

// ---------------- sidebar ----------------

async function refreshSidebar(selectAfter) {
  const data = await api('/api/tables');
  schema = data.tables;
  tableList.innerHTML = '';
  schema.forEach(t => {
    const li = document.createElement('li');
    li.textContent = t.name;
    li.className = t.name === activeTable ? 'active' : '';
    li.onclick = () => selectTable(t.name);
    tableList.appendChild(li);
  });
  if (selectAfter) selectTable(selectAfter);
}

function setActiveNav(name) {
  activeTable = name;
  [...tableList.children].forEach(li => li.classList.toggle('active', li.textContent === name));
}

// ---------------- grid view ----------------

async function selectTable(name) {
  setActiveNav(name);
  const def = schema.find(t => t.name === name);
  const data = await api(`/api/tables/${encodeURIComponent(name)}/rows`);
  renderGrid(def, data);
}

function badgesForColumn(c) {
  let out = '';
  if (c.primary_key) out += '<span class="badge pk">PK</span>';
  if (c.not_null) out += '<span class="badge nn">NN</span>';
  if (c.unique) out += '<span class="badge uq">UQ</span>';
  if (c.has_fk) out += `<span class="badge fk" title="references ${escapeHtml(c.fk_table)}.${escapeHtml(c.fk_column)}">FK</span>`;
  return out;
}

async function buildFkOptions(fkTable, fkColumn) {
  try {
    const rowsData = await api(`/api/tables/${encodeURIComponent(fkTable)}/rows`);
    const def = schema.find(t => t.name === fkTable);
    const colIdx = def.columns.findIndex(c => c.name === fkColumn);
    const labelIdx = def.columns.findIndex((c, i) => i !== colIdx && c.type === 'TEXT');
    return rowsData.rows.map(r => {
      const val = r.values[colIdx];
      const label = labelIdx >= 0 ? `${val} (${r.values[labelIdx]})` : String(val);
      return { value: val, label };
    });
  } catch (e) { return []; }
}

async function renderGrid(def, data) {
  const rowCount = data.rows.length;
  let html = `
    <h1>${escapeHtml(def.name)}</h1>
    <div class="subtitle">${rowCount} row${rowCount === 1 ? '' : 's'}</div>
    <div class="toolbar">
      <button class="danger" id="btn-drop-table">Delete table</button>
    </div>
    <table class="grid">
      <thead><tr><th class="rowid-col">#</th>${def.columns.map(c => `<th>${escapeHtml(c.name)}${badgesForColumn(c)}</th>`).join('')}<th></th></tr></thead>
      <tbody>
        ${data.rows.map(row => `
          <tr data-rowid="${row.rowid}">
            <td class="rowid-col">${row.rowid}</td>
            ${def.columns.map((c, i) => `<td><input data-col="${escapeHtml(c.name)}" value="${escapeHtml(row.values[i] === null ? '' : row.values[i])}"></td>`).join('')}
            <td class="actions"><button class="icon-btn" data-del="${row.rowid}" title="delete row">&times;</button></td>
          </tr>`).join('')}
      </tbody>
    </table>
    <h2>Add row</h2>
    <div class="add-row-form" id="add-row-form"></div>
    <button id="btn-add-row" style="margin-top:10px;">Add row</button>
  `;
  content.innerHTML = html;

  content.querySelector('#btn-drop-table').onclick = async () => {
    if (!confirm(`Delete table '${def.name}' and all its rows?`)) return;
    try {
      await api(`/api/tables/${encodeURIComponent(def.name)}`, { method: 'DELETE' });
      activeTable = null;
      content.innerHTML = '<div class="empty-state">Table deleted.</div>';
      refreshSidebar();
    } catch (e) { banner('error', e.message); }
  };

  content.querySelectorAll('[data-del]').forEach(btn => {
    btn.onclick = async () => {
      if (!confirm('Delete this row?')) return;
      try {
        await api(`/api/tables/${encodeURIComponent(def.name)}/rows/${btn.dataset.del}`, { method: 'DELETE' });
        selectTable(def.name);
      } catch (e) { banner('error', e.message); }
    };
  });

  content.querySelectorAll('tbody input').forEach(input => {
    input.addEventListener('change', async () => {
      const tr = input.closest('tr');
      const rowid = tr.dataset.rowid;
      const values = {};
      values[input.dataset.col] = coerceValue(input.value, def.columns.find(c => c.name === input.dataset.col).type);
      try {
        await api(`/api/tables/${encodeURIComponent(def.name)}/rows/${rowid}`, {
          method: 'PATCH', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ values })
        });
      } catch (e) { banner('error', e.message); selectTable(def.name); }
    });
  });

  // add-row form, with FK columns rendered as dropdowns instead of free text
  const form = content.querySelector('#add-row-form');
  for (const c of def.columns) {
    const wrap = document.createElement('div');
    wrap.className = 'field';
    const label = document.createElement('span');
    label.className = 'field-label';
    label.textContent = `${c.name} (${c.type})`;
    wrap.appendChild(label);
    if (c.has_fk) {
      const select = document.createElement('select');
      select.dataset.col = c.name;
      select.dataset.type = c.type;
      select.innerHTML = '<option value="">-- none --</option>';
      const opts = await buildFkOptions(c.fk_table, c.fk_column);
      opts.forEach(o => {
        const opt = document.createElement('option');
        opt.value = o.value; opt.textContent = o.label;
        select.appendChild(opt);
      });
      wrap.appendChild(select);
    } else {
      const input = document.createElement('input');
      input.dataset.col = c.name;
      input.dataset.type = c.type;
      input.type = (c.type === 'INT' || c.type === 'REAL') ? 'number' : 'text';
      if (c.type === 'REAL') input.step = 'any';
      wrap.appendChild(input);
    }
    form.appendChild(wrap);
  }

  content.querySelector('#btn-add-row').onclick = async () => {
    const values = {};
    form.querySelectorAll('[data-col]').forEach(el => {
      if (el.value !== '') values[el.dataset.col] = coerceValue(el.value, el.dataset.type);
    });
    try {
      await api(`/api/tables/${encodeURIComponent(def.name)}/rows`, {
        method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ values })
      });
      selectTable(def.name);
    } catch (e) { banner('error', e.message); }
  };
}

function coerceValue(raw, type) {
  if (type === 'INT') return parseInt(raw, 10);
  if (type === 'REAL') return parseFloat(raw);
  return raw;
}

// ---------------- create table designer ----------------

function renderCreateTableForm() {
  activeTable = null;
  setActiveNav(null);
  content.innerHTML = `
    <h1>New table</h1>
    <div class="subtitle">Click to add columns and constraints -- no SQL required.</div>
    <div class="field" style="margin-bottom:16px; max-width:300px;">
      <span class="field-label">Table name</span>
      <input id="new-table-name" placeholder="e.g. customers">
    </div>
    <h2>Columns</h2>
    <div id="col-rows"></div>
    <button class="secondary" id="btn-add-col" type="button">+ Add column</button>
    <div class="toolbar" style="margin-top:20px;">
      <button id="btn-create-table">Create table</button>
    </div>
  `;
  const colRows = content.querySelector('#col-rows');
  addColumnRow(colRows);

  content.querySelector('#btn-add-col').onclick = () => addColumnRow(colRows);

  content.querySelector('#btn-create-table').onclick = async () => {
    const name = content.querySelector('#new-table-name').value.trim();
    if (!name) { banner('error', 'table name is required'); return; }
    const columns = [...colRows.children].map(row => {
      const q = sel => row.querySelector(sel);
      const fkTable = q('.fk-table').value;
      const fkColumn = q('.fk-column').value;
      return {
        name: q('.col-name').value.trim(),
        type: q('.col-type').value,
        primary_key: q('.col-pk').checked,
        not_null: q('.col-nn').checked,
        unique: q('.col-uq').checked,
        fk_table: fkTable || undefined,
        fk_column: fkTable ? fkColumn : undefined,
      };
    }).filter(c => c.name);
    if (!columns.length) { banner('error', 'add at least one column'); return; }
    try {
      await api('/api/tables', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ name, columns }) });
      await refreshSidebar(name);
      banner('ok', `table '${name}' created`);
    } catch (e) { banner('error', e.message); }
  };
}

function addColumnRow(container) {
  const row = document.createElement('div');
  row.className = 'col-row';
  row.innerHTML = `
    <input class="col-name" placeholder="column name" style="width:140px;">
    <select class="col-type">
      <option value="INT">INT</option>
      <option value="REAL">REAL</option>
      <option value="TEXT">TEXT</option>
    </select>
    <label><input type="checkbox" class="col-pk"> PK</label>
    <label><input type="checkbox" class="col-nn"> NOT NULL</label>
    <label><input type="checkbox" class="col-uq"> UNIQUE</label>
    <span class="field-label">references</span>
    <select class="fk-table"><option value="">-- none --</option>${schema.map(t => `<option value="${escapeHtml(t.name)}">${escapeHtml(t.name)}</option>`).join('')}</select>
    <select class="fk-column"><option value="">--</option></select>
    <button type="button" class="icon-btn" title="remove column">&times;</button>
  `;
  row.querySelector('.fk-table').onchange = (e) => {
    const t = schema.find(t => t.name === e.target.value);
    const colSel = row.querySelector('.fk-column');
    colSel.innerHTML = t ? t.columns.map(c => `<option value="${escapeHtml(c.name)}">${escapeHtml(c.name)}</option>`).join('') : '<option value="">--</option>';
  };
  row.querySelector('.icon-btn').onclick = () => row.remove();
  container.appendChild(row);
}

// ---------------- schema view ----------------

function renderErd(tables) {
  const cardW = 220, colH = 18, headerH = 30, gapX = 60, gapY = 40, marginX = 20, marginY = 20;
  const perRow = Math.min(3, Math.max(1, tables.length));
  const positions = {};
  tables.forEach((t, i) => {
    const row = Math.floor(i / perRow), col = i % perRow;
    const h = headerH + t.columns.length * colH + 10;
    positions[t.name] = { x: marginX + col * (cardW + gapX), y: marginY + row * (200 + gapY), w: cardW, h, table: t };
  });
  const rows = Math.ceil(tables.length / perRow);
  const containerW = marginX * 2 + perRow * cardW + (perRow - 1) * gapX;
  const containerH = marginY * 2 + rows * (200 + gapY);

  let cardsHtml = '';
  Object.values(positions).forEach(p => {
    const colsHtml = p.table.columns.map(c => {
      const flags = [c.primary_key && 'PK', c.has_fk && 'FK', c.unique && 'UQ'].filter(Boolean).join(' ');
      return `<div class="erd-col ${c.primary_key ? 'pk' : ''}">${escapeHtml(c.name)} <span class="erd-type">${escapeHtml(c.type)}</span> ${flags ? `<span class="erd-flag">${flags}</span>` : ''}</div>`;
    }).join('');
    cardsHtml += `<div class="erd-card" style="left:${p.x}px; top:${p.y}px; width:${p.w}px;">
      <div class="erd-card-title">${escapeHtml(p.table.name)}</div>
      ${colsHtml}
    </div>`;
  });

  let linesHtml = '';
  tables.forEach(t => {
    t.columns.forEach((c, idx) => {
      if (c.has_fk && positions[c.fk_table]) {
        const src = positions[t.name], dst = positions[c.fk_table];
        const y1 = src.y + headerH + idx * colH + colH / 2;
        const x1 = src.x + (dst.x > src.x ? src.w : 0);
        const x2 = dst.x + (dst.x > src.x ? 0 : dst.w);
        const y2 = dst.y + dst.h / 2;
        linesHtml += `<line x1="${x1}" y1="${y1}" x2="${x2}" y2="${y2}" stroke="#5b8cff" stroke-width="1.5" marker-end="url(#arrow)" />`;
      }
    });
  });

  return `
    <div class="erd-wrap" style="position:relative; width:${Math.max(containerW, 300)}px; height:${containerH}px;">
      <svg width="${containerW}" height="${containerH}" style="position:absolute; left:0; top:0; pointer-events:none;">
        <defs><marker id="arrow" markerWidth="8" markerHeight="8" refX="6" refY="3" orient="auto"><path d="M0,0 L6,3 L0,6 Z" fill="#5b8cff"/></marker></defs>
        ${linesHtml}
      </svg>
      ${cardsHtml}
    </div>`;
}

async function renderSchemaView() {
  activeTable = null; setActiveNav(null);
  const data = await api('/api/schema');
  const tables = data.tables;
  let refHtml = tables.map(t => `
    <h2>${escapeHtml(t.name)}</h2>
    <table class="grid">
      <thead><tr><th>Column</th><th>Type</th><th>Constraints</th></tr></thead>
      <tbody>${t.columns.map(c => `<tr><td>${escapeHtml(c.name)}</td><td>${escapeHtml(c.type)}</td><td>${badgesForColumn(c)}</td></tr>`).join('')}</tbody>
    </table>
  `).join('');

  content.innerHTML = `
    <h1>Schema</h1>
    <div class="subtitle">Live, generated from the catalog -- click the button any time to re-fetch.</div>
    <div class="toolbar"><button class="secondary" id="btn-refresh-schema">Refresh</button></div>
    <h2 style="margin-top:0;">Diagram</h2>
    ${tables.length ? renderErd(tables) : '<div class="empty-state">No tables yet.</div>'}
    ${refHtml}
  `;
  content.querySelector('#btn-refresh-schema').onclick = renderSchemaView;
}

// ---------------- AI export view ----------------

async function renderExportView() {
  activeTable = null; setActiveNav(null);
  content.innerHTML = `<h1>AI-agent export</h1><div class="subtitle">Loading...</div>`;
  const [jsonRes, mdRes] = await Promise.all([
    fetch('/api/export.json').then(r => r.text()),
    fetch('/api/export.md').then(r => r.text()),
  ]);
  content.innerHTML = `
    <h1>AI-agent export</h1>
    <div class="subtitle">Schema, row counts, and sample rows in a format meant to be handed straight to an agent.</div>
    <div class="toolbar">
      <button id="btn-dl-json">Download schema.json</button>
      <button class="secondary" id="btn-dl-md">Download schema.md</button>
    </div>
    <h2>Markdown</h2>
    <pre class="export-block">${escapeHtml(mdRes)}</pre>
    <h2>JSON</h2>
    <pre class="export-block">${escapeHtml(jsonRes)}</pre>
  `;
  const download = (filename, text, mime) => {
    const blob = new Blob([text], { type: mime });
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob); a.download = filename; a.click();
    URL.revokeObjectURL(a.href);
  };
  content.querySelector('#btn-dl-json').onclick = () => download('schema.json', jsonRes, 'application/json');
  content.querySelector('#btn-dl-md').onclick = () => download('schema.md', mdRes, 'text/markdown');
}

// ---------------- advanced SQL (optional escape hatch) ----------------

function renderAdvancedSql() {
  activeTable = null; setActiveNav(null);
  content.innerHTML = `
    <h1>Advanced SQL</h1>
    <div class="subtitle">Optional escape hatch -- everything else in this app works without typing SQL.</div>
    <textarea class="sql-box" id="sql-box" placeholder="SELECT * FROM users WHERE age > 18 ORDER BY age DESC"></textarea>
    <div class="toolbar" style="margin-top:10px;"><button id="btn-run-sql">Run</button></div>
    <div id="sql-result"></div>
  `;
  content.querySelector('#btn-run-sql').onclick = async () => {
    const sql = content.querySelector('#sql-box').value;
    const resultDiv = content.querySelector('#sql-result');
    try {
      const r = await api('/api/query', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ sql }) });
      if (r.columns && r.columns.length) {
        resultDiv.innerHTML = `
          <div class="ok-banner">${escapeHtml(r.message || '')} (${r.affected} row(s))</div>
          <table class="grid">
            <thead><tr>${r.columns.map(c => `<th>${escapeHtml(c)}</th>`).join('')}</tr></thead>
            <tbody>${r.rows.map(row => `<tr>${row.map(v => `<td>${v === null ? '<i>NULL</i>' : escapeHtml(v)}</td>`).join('')}</tr>`).join('')}</tbody>
          </table>`;
      } else {
        resultDiv.innerHTML = `<div class="ok-banner">${escapeHtml(r.message || 'done')} (${r.affected} row(s) affected)</div>`;
        refreshSidebar();
      }
    } catch (e) {
      resultDiv.innerHTML = `<div class="error-banner">${escapeHtml(e.message)}</div>`;
    }
  };
}

// ---------------- wiring ----------------

document.getElementById('btn-new-table').onclick = renderCreateTableForm;
document.getElementById('btn-schema').onclick = renderSchemaView;
document.getElementById('btn-export').onclick = renderExportView;
document.getElementById('btn-advanced').onclick = renderAdvancedSql;

refreshSidebar();
