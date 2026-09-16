// 更新日志页：从 GitHub API 分页拉取 Release，渲染为时间线
// 每页 10 个，触底自动加载（按钮也可手动点击），直到没有更早的版本
const timeline = document.getElementById('timeline');
const statusEl = document.getElementById('releasesStatus');
const toc = document.getElementById('releasesToc');

const PER_PAGE = 10;
let page = 1;
let loading = false;
let exhausted = false;
let totalLoaded = 0;

function escapeHtml(s) {
  return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
          .replace(/"/g, '&quot;').replace(/'/g, '&#39;');
}

// 轻量 Markdown 渲染：标题、加粗、行内代码、链接、无序列表、段落
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

// 版本目录滚动高亮（所有已加载的卡片统一监听）
const tocSpy = new IntersectionObserver(entries => {
  for (const e of entries) {
    if (!e.isIntersecting) return;
    const tag = e.target.id.replace(/^rel-/, '');
    toc.querySelectorAll('a').forEach(a => a.classList.toggle('active', a.dataset.tag === tag));
  }
}, { rootMargin: '-20% 0px -70% 0px' });

function appendReleases(releases) {
  const itemsHtml = releases.map(r => {
    const isFirstOverall = totalLoaded === 0 && releases.indexOf(r) === 0;
    const badges = [
      isFirstOverall ? '<span class="rel-badge rel-badge-latest">最新</span>' : '',
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
  toc.insertAdjacentHTML('beforeend', releases.map(r =>
    `<a href="#rel-${r.tag_name}" data-tag="${r.tag_name}">${escapeHtml(r.tag_name)}</a>`
  ).join(''));

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

function loadMore() {
  if (loading || exhausted) return;
  loading = true;
  loadMoreBtn.textContent = '加载中…';
  loadMoreBtn.disabled = true;

  const finish = releases => {
    if (statusEl) statusEl.remove();
    if (releases.length === 0) {
      exhausted = true;
      loadMoreWrap.innerHTML = '<p class="load-more-end">已经到底啦，共 ' + totalLoaded + ' 个版本</p>';
      sentinelObserver.disconnect();
      return;
    }
    appendReleases(releases);
    page += 1;
    if (releases.length < PER_PAGE) {
      exhausted = true;
      loadMoreWrap.innerHTML = '<p class="load-more-end">已经到底啦，共 ' + totalLoaded + ' 个版本</p>';
      sentinelObserver.disconnect();
      return;
    }
    loadMoreBtn.textContent = '加载更早版本';
    loadMoreBtn.disabled = false;
  };
  const fail = () => {
    if (totalLoaded === 0) {
      statusEl.innerHTML = '加载失败，可能是网络问题或 GitHub 接口限流。可直接前往 ' +
        '<a href="https://github.com/H0308/qq-music-lyrics-displayer/releases" target="_blank" rel="noopener">GitHub Releases</a> 查看。';
      loadMoreWrap.remove();
      sentinelObserver.disconnect();
    } else {
      loadMoreBtn.textContent = '加载失败，点击重试';
      loadMoreBtn.disabled = false;
    }
  };

  const cacheKey = `gh-releases-p${page}`;
  const cached = ghCacheGet(cacheKey);
  if (cached) {
    finish(cached);
    loading = false;
    return;
  }

  fetch(`https://api.github.com/repos/H0308/qq-music-lyrics-displayer/releases?per_page=${PER_PAGE}&page=${page}`)
    .then(r => (r.ok ? r.json() : Promise.reject(r.status)))
    .then(releases => { ghCacheSet(cacheKey, releases); finish(releases); })
    .catch(fail)
    .finally(() => { loading = false; });
}

// 触底自动加载
const sentinelObserver = new IntersectionObserver(entries => {
  if (entries.some(e => e.isIntersecting)) loadMore();
}, { rootMargin: '400px 0px 0px 0px' });
sentinelObserver.observe(loadMoreWrap);

loadMoreBtn.addEventListener('click', loadMore);
loadMore();
