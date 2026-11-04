const docsSearch = document.getElementById('docsSearch');
const docsSearchStatus = document.getElementById('docsSearchStatus');
const docSections = Array.from(document.querySelectorAll('.doc-section'));
const docTocLinks = Array.from(document.querySelectorAll('#docsToc a'));
const docAnchors = docTocLinks.map(link => document.getElementById(link.hash.slice(1))).filter(Boolean);

const normalizeSearchText = text => text.toLocaleLowerCase('zh-CN').replace(/\s+/g, ' ').trim();

function highlightSearchMatches(section, query) {
  for (const mark of section.querySelectorAll('mark.doc-search-match')) {
    mark.replaceWith(document.createTextNode(mark.textContent));
  }
  section.normalize();
  if (!query) return;

  const walker = document.createTreeWalker(section, NodeFilter.SHOW_TEXT);
  const nodes = [];
  let content = '';
  while (walker.nextNode()) {
    const node = walker.currentNode;
    nodes.push({ node, start: content.length });
    content += node.textContent;
  }

  const pattern = query.replace(/[.*+?^${}()|[\]\\]/g, '\\$&').replace(/\s+/g, '\\s+');
  const ranges = Array.from(content.matchAll(new RegExp(pattern, 'gi')), match => ({
    start: match.index,
    end: match.index + match[0].length,
  }));

  for (const { node, start } of nodes) {
    const text = node.textContent;
    const matches = ranges.filter(range => range.start < start + text.length && range.end > start);
    if (!matches.length) continue;
    const fragment = document.createDocumentFragment();
    let offset = 0;
    for (const match of matches) {
      const begin = Math.max(0, match.start - start);
      const end = Math.min(text.length, match.end - start);
      fragment.append(document.createTextNode(text.slice(offset, begin)));
      const mark = document.createElement('mark');
      mark.className = 'doc-search-match';
      mark.textContent = text.slice(begin, end);
      fragment.append(mark);
      offset = end;
    }
    fragment.append(document.createTextNode(text.slice(offset)));
    node.replaceWith(fragment);
  }
}

docsSearch.addEventListener('input', () => {
  const query = normalizeSearchText(docsSearch.value);
  let matches = 0;

  for (const section of docSections) {
    const visible = !query || normalizeSearchText(section.textContent).includes(query);
    section.hidden = !visible;
    highlightSearchMatches(section, visible ? query : '');
    const link = docTocLinks.find(item => item.hash === `#${section.id}`);
    if (link) link.hidden = !visible;
    if (visible) matches += 1;
  }

  for (const group of document.querySelectorAll('[data-doc-group]')) {
    const heading = group.querySelector('.docs-toc-heading');
    const headingSection = document.getElementById(heading.hash.slice(1));
    const hasVisibleChild = Array.from(group.querySelectorAll('.docs-toc-children a'))
      .some(link => !link.hidden);
    group.hidden = Boolean(query) && !hasVisibleChild;
    if (headingSection) headingSection.hidden = Boolean(query) && !hasVisibleChild;
  }

  docsSearchStatus.textContent = query
    ? (matches ? `找到 ${matches} 个相关章节` : '没有找到相关内容，试试“歌词”或“频谱”')
    : '';

  scheduleActiveDocSectionUpdate(true);
});

function setActiveDocSection(id) {
  for (const link of docTocLinks) {
    link.classList.toggle('active', link.hash === `#${id}`);
  }
}

let activeDocUpdateTimer = 0;

function updateActiveDocSection() {
  const marker = 140;
  const pageBottom = window.scrollY + window.innerHeight >=
                     document.documentElement.scrollHeight - 2;
  let activeId = '';

  for (const anchor of docAnchors) {
    if (anchor.hidden) continue;
    if (anchor.getBoundingClientRect().top <= marker) activeId = anchor.id;
    else break;
  }

  if (pageBottom) {
    const lastVisible = docAnchors.findLast(anchor => !anchor.hidden);
    if (lastVisible) activeId = lastVisible.id;
  }

  if (activeId) setActiveDocSection(activeId);
}

function scheduleActiveDocSectionUpdate(immediate = false) {
  window.clearTimeout(activeDocUpdateTimer);
  activeDocUpdateTimer = window.setTimeout(updateActiveDocSection, immediate ? 0 : 80);
}

window.addEventListener('scroll', () => scheduleActiveDocSectionUpdate(), { passive: true });

for (const link of docTocLinks) {
  link.addEventListener('click', () => {
    setActiveDocSection(link.hash.slice(1));
    scheduleActiveDocSectionUpdate();
  });
}

updateActiveDocSection();
