// 主题三态切换：跟随系统 → 浅色 → 深色（初始主题由 <head> 内联脚本设置）
const themeToggle = document.getElementById('themeToggle');
const root = document.documentElement;
const systemLight = matchMedia('(prefers-color-scheme: light)');

const themeLabels = { system: '跟随系统', light: '浅色', dark: '深色' };
const themeOrder = ['system', 'light', 'dark'];

function applyThemeMode(mode) {
  const resolved = mode === 'system' ? (systemLight.matches ? 'light' : 'dark') : mode;
  root.dataset.themeMode = mode;
  root.dataset.theme = resolved;
  if (mode === 'system') localStorage.removeItem('theme');
  else localStorage.setItem('theme', mode);
  themeToggle.title = themeToggle.ariaLabel = `主题：${themeLabels[mode]}（点击切换）`;
}

themeToggle.addEventListener('click', () => {
  const current = root.dataset.themeMode || 'system';
  applyThemeMode(themeOrder[(themeOrder.indexOf(current) + 1) % themeOrder.length]);
});

// 跟随系统模式下，系统主题变化时实时更新
systemLight.addEventListener('change', () => {
  if (root.dataset.themeMode === 'system')
    root.dataset.theme = systemLight.matches ? 'light' : 'dark';
});

applyThemeMode(root.dataset.themeMode || 'system');

// 头部滚动态
const header = document.getElementById('siteHeader');
addEventListener('scroll', () => {
  header.classList.toggle('scrolled', scrollY > 8);
}, { passive: true });

// 返回顶部：滚动超过一屏出现，外圈圆环显示滚动进度
const backToTop = document.getElementById('backToTop');
const ringProgress = document.getElementById('ringProgress');
const RING_LEN = 2 * Math.PI * 21;

function updateBackToTop() {
  const max = document.documentElement.scrollHeight - innerHeight;
  const progress = max > 0 ? Math.min(scrollY / max, 1) : 0;
  ringProgress.style.strokeDashoffset = RING_LEN * (1 - progress);
  backToTop.classList.toggle('visible', scrollY > innerHeight * 0.6);
}

addEventListener('scroll', updateBackToTop, { passive: true });
updateBackToTop();

backToTop.addEventListener('click', () => {
  const smooth = !matchMedia('(prefers-reduced-motion: reduce)').matches;
  scrollTo({ top: 0, behavior: smooth ? 'smooth' : 'auto' });
});

// 卡片鼠标追踪光斑（亮点卡片 + 更多功能卡片）
const finePointer = matchMedia('(pointer: fine)').matches;
if (finePointer) {
  document.querySelectorAll('.hl, .explore-item').forEach(card => {
    card.addEventListener('mousemove', e => {
      const rect = card.getBoundingClientRect();
      card.style.setProperty('--mx', `${e.clientX - rect.left}px`);
      card.style.setProperty('--my', `${e.clientY - rect.top}px`);
    });
  });
}

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

// 动态获取最新 Release 版本号（失败则保持隐藏，不影响页面）
{
  const cacheKey = 'gh-latest-release';
  const showVersion = data => {
    const span = document.getElementById('latestVersion');
    const wrap = document.getElementById('heroVersion');
    // 这两个元素只在首页存在，其他页面（如更新日志）直接跳过，
    // 避免缓存命中时同步抛异常阻断本文件后续逻辑
    if (!data.tag_name || !span || !wrap) return;
    span.textContent = data.tag_name;
    wrap.hidden = false;
  };
  const cached = ghCacheGet(cacheKey);
  if (cached) {
    showVersion(cached);
  } else {
    fetch('https://api.github.com/repos/H0308/qq-music-lyrics-displayer/releases/latest')
      .then(r => (r.ok ? r.json() : Promise.reject(r.status)))
      .then(data => { ghCacheSet(cacheKey, data); showVersion(data); })
      .catch(() => {});
  }
}

// 入场动画
const io = new IntersectionObserver(entries => {
  for (const e of entries) {
    if (e.isIntersecting) {
      e.target.classList.add('visible');
      io.unobserve(e.target);
    }
  }
}, { threshold: 0.12 });

document.querySelectorAll('.reveal').forEach(el => io.observe(el));
