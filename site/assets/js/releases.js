// 更新日志页：GitHub API 分页拉取 Release
// 时间线：每页 10 个，触底自动加载（按钮可手动点击）
// 版本目录：独立按页请求，上一页/下一页翻页；点击未加载版本时自动补载时间线再跳转
const timeline = document.getElementById('timeline');
const statusEl = document.getElementById('releasesStatus');
const toc = document.getElementById('releasesToc');

const PER_PAGE = 10;

// 时间线状态
let nextTimelinePage = 1;
let timelineLoading = false;
let timelineExhausted = false;
let totalLoaded = 0;

// 目录状态
let tocPage = 1;
let tocLoading = false;
let tocExhausted = false;

// GitHub API 响应缓存（30 分钟），避免刷新/重复访问消耗未认证限流额度（60 次/小时/IP）
function ghCacheGet(key) {
  try {
    const raw = localStorage.getItem(key);
    if (!raw) return null;
    const { time, data } = JSON.parse(raw);
    return Date.now() - time < 30 * 60 * 1000 ? data : null;
  } catch { return null; }
}
function ghCacheSet(key, data) {
  try { localStorage.setItem(key, JSON.stringify({ time: Date.now(), data })); } catch { /* 隐私模式等场景忽略 */ }
}

async function fetchReleasePage(pageNum) {
  const cacheKey = `gh-releases-p${pageNum}`;
  const cached = ghCacheGet(cacheKey);
  if (cached) return cached;
  const r = await fetch(
    `https://api.github.com/repos/H0308/qq-music-lyrics-displayer/releases?per_page=${PER_PAGE}&page=${pageNum}`);
  if (!r.ok) throw r.status;
  const data = await r.json();
  ghCacheSet(cacheKey, data);
  return data;
}

function escapeHtml(s) {
  return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
          .replace(/"/g, '&quot;').replace(/'/g, '&#39;');
}

// 轻量 Markdown 渲染：标题、加粗、行内代码、链接、无序列表、引用块、段落
function mdToHtml(md) {
  const lines = escapeHtml(md).split(/\r?\n/);
  let html = '';
  let inList = false;
  const inline = s => s
    .replace(/\*\*(.+?)\*\*/g, '<strong>$1</strong>')
    .replace(/`([^`]+)`/g, '<code>$1</code>')
    .replace(/\[([^\]]+)\]\((https?:[^)\s]+)\)/g,
             '<a href="$2" target="_blank" rel="noopener">$1</a>');

  for (const raw of lines) {
    const line = raw.trim();
    if (/^#{1,4}\s+/.test(line)) {
      if (inList) { html += '</ul>'; inList = false; }
      html += `<h4>${inline(line.replace(/^#{1,4}\s+/, ''))}</h4>`;
    } else if (/^&gt;\s*/.test(line)) {
      // 引用块（行首 > 已在转义后变为 &gt;）
      if (inList) { html += '</ul>'; inList = false; }
      html += `<blockquote>${inline(line.replace(/^&gt;\s*/, ''))}</blockquote>`;
    } else if (/^[-*]\s+/.test(line)) {
      if (!inList) { html += '<ul>'; inList = true; }
      html += `<li>${inline(line.replace(/^[-*]\s+/, ''))}</li>`;
    } else if (line) {
      if (inList) { html += '</ul>'; inList = false; }
      html += `<p>${inline(line)}</p>`;
    }
  }
  if (inList) html += '</ul>';
  return html;
}

function formatDate(iso) {
  const d = new Date(iso);
  return `${d.getFullYear()} 年 ${d.getMonth() + 1} 月 ${d.getDate()} 日`;
}

// ===== 时间线 =====

// 目录高亮监听（目录链接按页重绘，每次重绘后重新查询）
const tocSpy = new IntersectionObserver(entries => {
  for (const e of entries) {
    if (!e.isIntersecting) return;
    const tag = e.target.id.replace(/^rel-/, '');
    toc.querySelectorAll('a').forEach(a => a.classList.toggle('active', a.dataset.tag === tag));
  }
}, { rootMargin: '-20% 0px -70% 0px' });

function appendReleases(releases) {
  const startIndex = totalLoaded;
  const itemsHtml = releases.map((r, i) => {
    const badges = [
      startIndex === 0 && i === 0 ? '<span class="rel-badge rel-badge-latest">最新</span>' : '',
      r.prerelease ? '<span class="rel-badge rel-badge-pre">预发布</span>' : '',
    ].join('');
    const title = r.name && r.name !== r.tag_name ? `${r.tag_name} · ${escapeHtml(r.name)}` : r.tag_name;
    const body = r.body ? mdToHtml(r.body) : '<p class="rel-empty">该版本未填写更新说明。</p>';
    return `
      <article class="release-item reveal" id="rel-${r.tag_name}">
        <div class="release-head">
          <h2>${title}</h2>${badges}
        </div>
        <p class="release-date">${formatDate(r.published_at)}</p>
        <div class="release-body">${body}</div>
        <a class="release-link" href="${r.html_url}" target="_blank" rel="noopener">查看该版本与下载 →</a>
      </article>`;
  }).join('');

  timeline.insertAdjacentHTML('beforeend', itemsHtml);

  // 新卡片加入入场动画与目录高亮监听
  timeline.querySelectorAll('.release-item:not(.reveal-watched)').forEach(el => {
    el.classList.add('reveal-watched');
    io.observe(el);
    tocSpy.observe(el);
  });
  totalLoaded += releases.length;
}

// 底部加载区：按钮同时充当触底哨兵
const loadMoreWrap = document.createElement('div');
loadMoreWrap.className = 'load-more';
loadMoreWrap.innerHTML = '<button class="btn btn-ghost" id="loadMoreBtn" type="button">加载更早版本</button>';
timeline.after(loadMoreWrap);
const loadMoreBtn = loadMoreWrap.querySelector('#loadMoreBtn');

async function loadTimelineMore() {
  if (timelineLoading || timelineExhausted) return;
  timelineLoading = true;
  loadMoreBtn.textContent = '加载中…';
  loadMoreBtn.disabled = true;
  try {
    const releases = await fetchReleasePage(nextTimelinePage);
    if (statusEl) statusEl.remove();
    if (releases.length === 0) {
      timelineExhausted = true;
      loadMoreWrap.innerHTML = '<p class="load-more-end">已经到底啦，共 ' + totalLoaded + ' 个版本</p>';
      sentinelObserver.disconnect();
      return;
    }
    appendReleases(releases);
    nextTimelinePage += 1;
    if (releases.length < PER_PAGE) {
      timelineExhausted = true;
      loadMoreWrap.innerHTML = '<p class="load-more-end">已经到底啦，共 ' + totalLoaded + ' 个版本</p>';
      sentinelObserver.disconnect();
      return;
    }
    loadMoreBtn.textContent = '加载更早版本';
    loadMoreBtn.disabled = false;
  } catch {
    if (totalLoaded === 0) {
      statusEl.innerHTML = '加载失败，可能是网络问题或 GitHub 接口限流。可直接前往 ' +
        '<a href="https://github.com/H0308/qq-music-lyrics-displayer/releases" target="_blank" rel="noopener">GitHub Releases</a> 查看。';
      loadMoreWrap.remove();
      sentinelObserver.disconnect();
    } else {
      loadMoreBtn.textContent = '加载失败，点击重试';
      loadMoreBtn.disabled = false;
    }
  } finally {
    timelineLoading = false;
  }
}

// 触底自动加载
const sentinelObserver = new IntersectionObserver(entries => {
  if (entries.some(e => e.isIntersecting)) loadTimelineMore();
}, { rootMargin: '400px 0px 0px 0px' });
sentinelObserver.observe(loadMoreWrap);
loadMoreBtn.addEventListener('click', loadTimelineMore);

// ===== 版本目录（独立分页） =====

function renderToc(releases) {
  toc.innerHTML = releases.map(r =>
    `<a href="#rel-${r.tag_name}" data-tag="${r.tag_name}">${escapeHtml(r.tag_name)}</a>`
  ).join('') + `
    <div class="toc-pager">
      <button type="button" data-nav="prev" ${tocPage === 1 ? 'disabled' : ''}>上一页</button>
      <span>第 ${tocPage} 页</span>
      <button type="button" data-nav="next" ${tocExhausted ? 'disabled' : ''}>下一页</button>
    </div>`;
}

async function showTocPage(pageNum) {
  if (tocLoading || pageNum < 1) return;
  tocLoading = true;
  try {
    const releases = await fetchReleasePage(pageNum);
    if (releases.length === 0) {
      tocExhausted = true; // 没有更多页，停留在当前页
      renderTocCurrentState();
      return;
    }
    tocPage = pageNum;
    tocExhausted = releases.length < PER_PAGE;
    renderToc(releases);
  } catch {
    toc.innerHTML = '<p class="toc-error">目录加载失败，<a href="#" data-nav="retry">点击重试</a></p>';
  } finally {
    tocLoading = false;
  }
}

// 到底后重绘翻页器（禁用下一页），保持当前页链接不变
function renderTocCurrentState() {
  const pager = toc.querySelector('.toc-pager');
  if (!pager) return;
  pager.querySelector('[data-nav="next"]').disabled = true;
}

// 目录点击：翻页 / 重试 / 版本跳转（未加载的版本先补载时间线）
toc.addEventListener('click', async e => {
  const navBtn = e.target.closest('[data-nav]');
  if (navBtn) {
    e.preventDefault();
    const nav = navBtn.dataset.nav;
    if (nav === 'prev') showTocPage(tocPage - 1);
    else if (nav === 'next') showTocPage(tocPage + 1);
    else if (nav === 'retry') showTocPage(tocPage);
    return;
  }

  const link = e.target.closest('a[data-tag]');
  if (!link) return;
  const targetId = 'rel-' + link.dataset.tag;
  if (document.getElementById(targetId)) {
    closeTocDrawer(); // 已加载：走原生锚点跳转
    return;
  }
  // 未加载：自动补载时间线，直到该版本出现或全部加载完
  e.preventDefault();
  link.classList.add('toc-loading');
  while (!timelineExhausted && !document.getElementById(targetId))
    await loadTimelineMore();
  link.classList.remove('toc-loading');
  const target = document.getElementById(targetId);
  if (target) target.scrollIntoView({ behavior: 'smooth' });
  closeTocDrawer();
});

// ===== 移动端抽屉 =====

const tocFab = document.getElementById('tocFab');
let tocBackdrop = null;
if (tocFab) {
  tocBackdrop = document.createElement('div');
  tocBackdrop.className = 'toc-backdrop';
  document.body.appendChild(tocBackdrop);
  tocFab.addEventListener('click', () => {
    const open = toc.classList.toggle('open');
    tocBackdrop.classList.toggle('open', open);
    tocFab.classList.toggle('fab-hidden', open);
  });
  tocBackdrop.addEventListener('click', closeTocDrawer);
}
function closeTocDrawer() {
  toc.classList.remove('open');
  if (tocBackdrop) tocBackdrop.classList.remove('open');
  if (tocFab) tocFab.classList.remove('fab-hidden');
}

// ===== 初始化 =====
loadTimelineMore();
showTocPage(1);
