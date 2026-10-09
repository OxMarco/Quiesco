// Holds a section's animations until it scrolls into view. `armed` is added at
// once, so without JavaScript nothing is hidden; `inview` is added when the
// element is seen. With `once` off, `inview` is removed again when it leaves,
// which lets looping animations pause off-screen.

export function playWhenSeen(els: Iterable<Element>, once = true) {
  const io = new IntersectionObserver(
    (entries) => {
      for (const e of entries) {
        e.target.classList.toggle('inview', e.isIntersecting || (once && e.target.classList.contains('inview')));
        if (once && e.isIntersecting) io.unobserve(e.target);
      }
    },
    { threshold: 0.35 },
  );
  for (const el of els) {
    el.classList.add('armed');
    io.observe(el);
  }
}

/** Restarts the CSS animations under `el` by toggling `inview` across a reflow. */
export function replay(el: HTMLElement) {
  el.classList.remove('inview');
  void el.offsetWidth;
  el.classList.add('inview');
}

export const reducedMotion = () => window.matchMedia('(prefers-reduced-motion: reduce)').matches;
