// 网站氛围宠物：Dock 模式小宠物的网页版"形象大使"。
// 帧素材与行为节奏移植自 src/ui/dock_pet.cpp（行走漫游/坐/张望/调整耳机/
// 挥手/瞌睡/唱歌跳舞彩蛋），图集由 site/tools/build_pet_sprites.py 生成。
// 仅桌面宽度（>640px）显示；窄屏不初始化。
(function () {
  'use strict';

  // ---- 帧表（图集像素坐标，build_pet_sprites.py 生成时打印）----
  var ATLAS_W = 356;
  var ATLAS_H = 385;
  var FRAMES = {
    walk1:  { x: 0,   y: 0,   w: 89, h: 88 },
    walk2:  { x: 89,  y: 0,   w: 88, h: 88 },
    wave1:  { x: 177, y: 0,   w: 89, h: 88 },
    wave2:  { x: 266, y: 0,   w: 89, h: 88 },
    stand:  { x: 0,   y: 88,  w: 93, h: 81 },
    lookup: { x: 93,  y: 88,  w: 99, h: 81 },
    adjust: { x: 192, y: 88,  w: 98, h: 81 },
    blink:  { x: 290, y: 88,  w: 27, h: 7  },
    sit:    { x: 0,   y: 169, w: 99, h: 72 },
    sing1:  { x: 99,  y: 169, w: 89, h: 71 },
    sing2:  { x: 188, y: 169, w: 89, h: 71 },
    happy:  { x: 0,   y: 241, w: 89, h: 71 },
    dance1: { x: 89,  y: 241, w: 89, h: 72 },
    dance2: { x: 178, y: 241, w: 89, h: 72 },
    dance3: { x: 267, y: 241, w: 89, h: 72 },
    yawn1:  { x: 0,   y: 313, w: 89, h: 72 },
    yawn2:  { x: 89,  y: 313, w: 89, h: 72 },
    sleep:  { x: 178, y: 313, w: 89, h: 72 }
  };

  // 行走帧眨眼时闭眼贴片的落点（CSS px，相对帧左上角；由 dock_pet.cpp 的
  // kWalkEyeTargets 按图集缩放 88/170、渲染缩放 0.5 换算）。lookup 同理。
  var BLINK_TARGET = {
    walk1: { l: 23.0, t: 20.2, w: 9.6,  h: 5.7 },
    walk2: { l: 23.3, t: 20.2, w: 11.9, h: 5.7 },
    lookup: { l: 22.8, t: 16.0, w: 11.4, h: 5.2 }
  };
  // 行走帧内容中心修正（dock_pet.cpp kWalkContentCenterX / kWalkAnchorX，
  // 锚 81.5，帧1=78 帧2=85），避免帧间身体横跳。
  var WALK_OFFSET_X = { walk1: 0.9, walk2: -0.9 };

  // ---- 节奏常量（毫秒，数值与桌面端一致）----
  var CSS_SCALE = 0.5;           // 图集按 2x 生成，基准渲染 0.5
  var WALK_FRAME_MS = 230;       // 踏步帧间隔
  var WALK_SPEED = 26;           // CSS px/s（桌面 12dip/s，网页车道更宽，略加快）
  var WALK_MIN_MS = 6500;        // 一段行走的最短时长
  var WALK_JITTER_MS = 5500;     // 行走时长随机增量
  var BLINK_MS = 130;            // 眨眼时长
  var WAVE_MS = 1800;            // 挥手问候时长
  var WAVE_FRAME_MS = 300;
  var YAWN_MS = 2800;            // 哈欠过渡时长
  var YAWN_FRAME_MS = 700;
  var SLEEP_AFTER_MS = 3 * 60 * 1000; // 无交互多久后打瞌睡
  var STAND_BREATH_MS = 2600;    // 站立/坐姿呼吸周期
  var SLEEP_BREATH_MS = 3400;    // 睡眠呼吸周期
  var ENCORE_HAPPY_MS = 700;     // 彩蛋开头爱心眼时长
  var ENCORE_STEP_MS = 260;      // 唱歌/跳舞步频
  var ENCORE_STEPS = 10;         // 彩蛋持续步数（约 2.6s）
  var LANE_PADDING = 30;         // 行走车道距视口边缘的距离

  var TWO_PI = Math.PI * 2;

  // 用户主动开了减弱动效：静态坐姿展示，不播放任何动画。
  var reduceMotion = matchMedia('(prefers-reduced-motion: reduce)').matches;
  var desktopQuery = matchMedia('(min-width: 641px)');

  var active = false; // 当前是否已初始化（宽屏才初始化）

  // DOM 引用（setup 时填充）
  var root, moveEl, flipEl, poseEl, bodyEl, blinkEl, zzzEl;

  // ---- 状态 ----
  // mode: wave(入场/唤醒问候) roam encore(点击彩蛋) yawn sleep
  var mode = 'wave';
  var behavior = 'stand'; // roam 子状态：walk stand sit lookup adjust
  var x = 0;              // 宠物中心 x（CSS px）
  var direction = 1;
  var currentFrame = '';
  var modeStart = 0;
  var behaviorStart = 0;
  var behaviorUntil = 0;
  var nextBlink = 0;
  var blinkUntil = 0;
  var encoreKind = 'sing';
  var lastActivity = 0;
  var lastTick = 0;
  var rafId = 0;

  // 把图集中的 frame 渲染到元素上，显示尺寸为图集像素 × scale。
  function setSprite(el, frame, scale) {
    el.style.width = frame.w * scale + 'px';
    el.style.height = frame.h * scale + 'px';
    el.style.backgroundSize = (ATLAS_W * scale) + 'px ' + (ATLAS_H * scale) + 'px';
    el.style.backgroundPosition = (-frame.x * scale) + 'px ' + (-frame.y * scale) + 'px';
  }

  function laneMin() { return LANE_PADDING; }
  function laneMax() { return Math.max(laneMin(), window.innerWidth - LANE_PADDING); }

  function randomBetween(min, max) { return min + Math.random() * (max - min); }

  function isNight() {
    var h = new Date().getHours();
    return h >= 23 || h < 7;
  }

  // roam 的下一步行为，概率分布与桌面端 chooseRoamingBehavior 一致
  // （夜间 23:00–07:00 更安静：久坐久看、少走动）。
  function chooseBehavior(now) {
    var night = isNight();
    var choice = Math.floor(Math.random() * 10);
    var sitBelow = night ? 4 : 2;
    var lookUpBelow = night ? 7 : 4;
    var adjustChoice = night ? 7 : 4;

    if (choice < sitBelow) {
      startBehavior('sit', now, night ? randomBetween(2800, 4600) : randomBetween(1800, 3000));
      return;
    }
    if (choice < lookUpBelow) {
      startBehavior('lookup', now, randomBetween(2200, 3400));
      nextBlink = now + randomBetween(900, 2000);
      return;
    }
    if (choice === adjustChoice) {
      startBehavior('adjust', now, 900);
      return;
    }
    startBehavior('walk', now, WALK_MIN_MS + Math.random() * WALK_JITTER_MS);
    if (x <= laneMin() + 1) direction = 1;
    else if (x >= laneMax() - 1) direction = -1;
    else direction = Math.random() < 0.5 ? -1 : 1;
    nextBlink = now + randomBetween(1600, 3800);
  }

  function startBehavior(name, now, durationMs) {
    behavior = name;
    behaviorStart = now;
    behaviorUntil = now + durationMs;
    blinkUntil = 0;
    if (name !== 'lookup' && name !== 'walk') nextBlink = 0;
  }

  function enterMode(next, now) {
    mode = next;
    modeStart = now;
    zzzEl.classList.toggle('visible', next === 'sleep');
    if (next === 'roam') {
      startBehavior('stand', now, randomBetween(500, 1200));
    }
  }

  // ---- 每帧更新 ----
  function tick(now) {
    rafId = requestAnimationFrame(tick);
    if (!lastTick) { lastTick = now; return; }
    var dt = Math.min((now - lastTick) / 1000, 0.1);
    lastTick = now;

    if (mode === 'wave') {
      setFrame(Math.floor(now / WAVE_FRAME_MS) % 2 === 0 ? 'wave1' : 'wave2');
      applyPose(0, 0, 1);
      if (now - modeStart >= WAVE_MS) enterMode('roam', now);
      return;
    }

    if (mode === 'encore') {
      var elapsed = now - modeStart;
      if (elapsed < ENCORE_HAPPY_MS) {
        setFrame('happy');
      } else {
        var step = Math.floor((elapsed - ENCORE_HAPPY_MS) / ENCORE_STEP_MS);
        if (step >= ENCORE_STEPS) {
          enterMode('roam', now);
          return;
        }
        if (encoreKind === 'sing') {
          setFrame(step % 2 === 0 ? 'sing1' : 'sing2');
        } else {
          // 桌面端跳舞循环：左倾-居中-右倾-居中。
          setFrame(['dance1', 'dance2', 'dance3', 'dance2'][step % 4]);
        }
      }
      applyPose(0, 0, breathScale(now, STAND_BREATH_MS, 0.02));
      return;
    }

    if (mode === 'yawn') {
      setFrame(Math.floor(now / YAWN_FRAME_MS) % 2 === 0 ? 'yawn1' : 'yawn2');
      applyPose(0, 0, 1);
      if (now - modeStart >= YAWN_MS) enterMode('sleep', now);
      return;
    }

    if (mode === 'sleep') {
      setFrame('sleep');
      applyPose(0, 0, breathScale(now, SLEEP_BREATH_MS, 0.028));
      return;
    }

    // ---- roam ----
    if (now - lastActivity >= SLEEP_AFTER_MS) {
      enterMode('yawn', now);
      return;
    }

    // 眨眼调度（walk / lookup 期间）。
    if (behavior === 'walk' || behavior === 'lookup') {
      if (blinkUntil && now >= blinkUntil) {
        blinkUntil = 0;
        nextBlink = now + randomBetween(1700, 4000);
      } else if (!blinkUntil && nextBlink && now >= nextBlink) {
        blinkUntil = now + BLINK_MS;
      }
    }

    if (behavior === 'walk') {
      x += direction * WALK_SPEED * dt;
      var hitLeft = x <= laneMin();
      var hitRight = x >= laneMax();
      if (hitLeft || hitRight || now >= behaviorUntil) {
        x = Math.min(Math.max(x, laneMin()), laneMax());
        if (hitLeft) direction = 1;
        else if (hitRight) direction = -1;
        startBehavior('stand', now, randomBetween(600, 1500));
      } else {
        var phase = Math.floor((now - behaviorStart) / WALK_FRAME_MS) % 2;
        var wf = phase === 0 ? 'walk1' : 'walk2';
        setFrame(wf, blinkUntil ? wf : null);
        // 踏步同源的上下颠簸与侧倾（dock_pet.cpp kWalkBobDip / kWalkRockDeg）。
        var stepPhase = ((now - behaviorStart) % WALK_FRAME_MS) / WALK_FRAME_MS;
        var bob = 0.8 * Math.abs(Math.sin(Math.PI * stepPhase));
        var rock = 3.5 * Math.sin(Math.PI * ((now - behaviorStart) / WALK_FRAME_MS));
        applyPose(bob, rock, 1);
        return;
      }
    }

    if (behavior === 'sit') {
      setFrame('sit'); // 坐姿底帧即闭眼打盹姿态，不叠加眨眼。
      applyPose(0, 0, breathScale(now, STAND_BREATH_MS, 0.02));
      if (now >= behaviorUntil) startBehavior('stand', now, randomBetween(500, 1200));
      return;
    }

    if (behavior === 'lookup') {
      setFrame('lookup', blinkUntil ? 'lookup' : null);
      applyPose(0, 0, 1);
      if (now >= behaviorUntil) startBehavior('stand', now, randomBetween(500, 1200));
      return;
    }

    if (behavior === 'adjust') {
      setFrame('adjust');
      applyPose(0, 0, 1);
      if (now >= behaviorUntil) startBehavior('stand', now, randomBetween(500, 1200));
      return;
    }

    // stand
    setFrame('stand');
    applyPose(0, 0, breathScale(now, STAND_BREATH_MS, 0.02));
    if (now >= behaviorUntil) chooseBehavior(now);
  }

  // 沿底部锚点的缓慢等比缩放，让静止姿态不完全冻住。
  function breathScale(now, periodMs, amount) {
    return 1 + amount * Math.sin(TWO_PI * ((now % periodMs) / periodMs));
  }

  function setFrame(name, blinkKey) {
    if (name !== currentFrame) {
      currentFrame = name;
      var f = FRAMES[name];
      setSprite(bodyEl, f, CSS_SCALE);
      moveEl.style.width = f.w * CSS_SCALE + 'px';
      moveEl.style.height = f.h * CSS_SCALE + 'px';
      flipEl.style.height = f.h * CSS_SCALE + 'px';
    }
    // 闭眼贴片：位置由帧对应的 BLINK_TARGET 给出，随身体一起倾/起伏。
    if (blinkKey && BLINK_TARGET[blinkKey]) {
      var t = BLINK_TARGET[blinkKey];
      var bf = FRAMES.blink;
      blinkEl.style.display = 'block';
      blinkEl.style.left = t.l + 'px';
      blinkEl.style.top = t.t + 'px';
      blinkEl.style.width = t.w + 'px';
      blinkEl.style.height = t.h + 'px';
      blinkEl.style.backgroundSize = (ATLAS_W * t.w / bf.w) + 'px ' + (ATLAS_H * t.h / bf.h) + 'px';
      blinkEl.style.backgroundPosition = (-bf.x * t.w / bf.w) + 'px ' + (-bf.y * t.h / bf.h) + 'px';
    } else {
      blinkEl.style.display = 'none';
    }
  }

  function applyPose(bobPx, rockDeg, scale) {
    var walkOffset = (behavior === 'walk' && WALK_OFFSET_X[currentFrame]) || 0;
    var bodyW = currentFrame ? FRAMES[currentFrame].w * CSS_SCALE : 0;
    moveEl.style.transform = 'translateX(' + (x - bodyW / 2 + walkOffset) + 'px)';
    flipEl.style.transform = direction < 0 ? 'scaleX(-1)' : '';
    poseEl.style.transform =
      'translateY(' + (-bobPx) + 'px) rotate(' + rockDeg + 'deg) scale(' + scale + ')';
  }

  // ---- 交互 ----
  function onPetTap() {
    var now = performance.now();
    lastActivity = now;
    if (reduceMotion) return;
    if (mode === 'sleep' || mode === 'yawn') {
      enterMode('wave', now); // 唤醒并挥手致意
      return;
    }
    if (mode === 'encore') return; // 彩蛋演出中不叠加
    encoreKind = Math.random() < 0.5 ? 'sing' : 'dance';
    enterMode('encore', now);
  }

  function onUserActivity() {
    var now = performance.now();
    lastActivity = now;
    if (!reduceMotion && (mode === 'sleep' || mode === 'yawn')) {
      enterMode('wave', now);
    }
  }

  function onDocumentClick(e) {
    if (!root.contains(e.target)) onUserActivity();
  }

  function onResize() {
    x = Math.min(Math.max(x, laneMin()), laneMax());
  }

  function setup() {
    if (active) return;
    active = true;

    root = document.createElement('div');
    root.className = 'site-pet';
    root.setAttribute('role', 'img');
    root.setAttribute('aria-label', '网站吉祥物：任务栏歌词的小宠物');
    root.innerHTML =
      '<div class="pet-move">' +
        '<div class="pet-flip">' +
          '<div class="pet-pose">' +
            '<div class="pet-body"><div class="pet-blink"></div></div>' +
          '</div>' +
          '<div class="pet-zzz" aria-hidden="true"><span>Z</span><span>Z</span><span>Z</span></div>' +
        '</div>' +
      '</div>';
    moveEl = root.querySelector('.pet-move');
    flipEl = root.querySelector('.pet-flip');
    poseEl = root.querySelector('.pet-pose');
    bodyEl = root.querySelector('.pet-body');
    blinkEl = root.querySelector('.pet-blink');
    zzzEl = root.querySelector('.pet-zzz');

    root.addEventListener('click', onPetTap);
    window.addEventListener('resize', onResize);
    // 鼠标移动不算"活动"（否则永远不会瞌睡），与桌面端"暂停 3 分钟入睡"对应。
    window.addEventListener('keydown', onUserActivity, { passive: true });
    window.addEventListener('scroll', onUserActivity, { passive: true });
    window.addEventListener('touchstart', onUserActivity, { passive: true });
    document.addEventListener('click', onDocumentClick);

    document.body.appendChild(root);
    root.classList.add('pet-ready');
    currentFrame = '';

    if (reduceMotion) {
      // 静态坐姿：无任何循环与交互动画。
      setFrame('sit');
      moveEl.style.transform = 'translateX(24px)';
      return;
    }

    lastTick = 0;
    lastActivity = performance.now();
    x = laneMin() + 40;
    direction = 1;

    // 同一会话只问候一次；之后直接站定开场。
    var greeted = false;
    try {
      greeted = sessionStorage.getItem('pet-greeted') === '1';
      if (!greeted) sessionStorage.setItem('pet-greeted', '1');
    } catch (e) { /* 隐私模式 */ }
    var now = performance.now();
    if (greeted) {
      enterMode('roam', now);
    } else {
      mode = 'wave';
      modeStart = now;
    }
    rafId = requestAnimationFrame(tick);
  }

  function teardown() {
    if (!active) return;
    active = false;
    cancelAnimationFrame(rafId);
    root.removeEventListener('click', onPetTap);
    window.removeEventListener('resize', onResize);
    window.removeEventListener('keydown', onUserActivity);
    window.removeEventListener('scroll', onUserActivity);
    window.removeEventListener('touchstart', onUserActivity);
    document.removeEventListener('click', onDocumentClick);
    root.remove();
  }

  // 图集加载完成后再显示，避免背景未就绪的闪烁。窄屏不初始化；
  // 运行中窗口跨过 640px 边界时实时挂载/移除。
  var atlas = new Image();
  atlas.onload = function () {
    if (desktopQuery.matches) setup();
    desktopQuery.addEventListener('change', function (e) {
      if (e.matches) setup();
      else teardown();
    });
  };
  atlas.src = 'assets/img/pet-sprites.webp';
})();
