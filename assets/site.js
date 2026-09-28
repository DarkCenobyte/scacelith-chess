'use strict';

// Links retain their normal image destinations if JavaScript is unavailable.
(() => {
  const dialog = document.querySelector('.lightbox');
  const links = Array.from(document.querySelectorAll('[data-gallery]'));
  if (!dialog || typeof dialog.showModal !== 'function' || !links.length) return;

  const image = dialog.querySelector('.lightbox-image');
  const title = dialog.querySelector('#lightbox-title');
  const caption = dialog.querySelector('#lightbox-caption');
  const counter = dialog.querySelector('.lightbox-counter');
  let current = 0;
  let opener = null;

  function display(index) {
    current = (index + links.length) % links.length;
    const link = links[current];
    image.src = link.href;
    image.alt = link.querySelector('img').alt;
    title.textContent = link.dataset.title;
    caption.textContent = link.dataset.caption;
    counter.textContent = `${current + 1} / ${links.length}`;
  }

  links.forEach((link, index) => {
    link.addEventListener('click', event => {
      if (event.ctrlKey || event.metaKey || event.shiftKey || event.altKey) return;
      event.preventDefault();
      opener = link;
      display(index);
      dialog.showModal();
      document.body.classList.add('modal-open');
    });
  });

  dialog.querySelector('.lightbox-close').addEventListener('click', () => dialog.close());
  dialog.querySelector('.lightbox-prev').addEventListener('click', () => display(current - 1));
  dialog.querySelector('.lightbox-next').addEventListener('click', () => display(current + 1));
  dialog.addEventListener('keydown', event => {
    if (event.key === 'ArrowLeft' || event.key === 'ArrowRight') {
      event.preventDefault();
      display(current + (event.key === 'ArrowLeft' ? -1 : 1));
    }
  });

  // Native dialog supplies focus trapping and Escape; only outside clicks close it.
  let startedOnBackdrop = false;
  dialog.addEventListener('pointerdown', event => {
    const bounds = dialog.getBoundingClientRect();
    startedOnBackdrop = event.target === dialog && (
      event.clientX < bounds.left || event.clientX > bounds.right ||
      event.clientY < bounds.top || event.clientY > bounds.bottom
    );
  });
  dialog.addEventListener('click', event => {
    const bounds = dialog.getBoundingClientRect();
    if (startedOnBackdrop && event.target === dialog && (
      event.clientX < bounds.left || event.clientX > bounds.right ||
      event.clientY < bounds.top || event.clientY > bounds.bottom
    )) dialog.close();
    startedOnBackdrop = false;
  });
  dialog.addEventListener('close', () => {
    document.body.classList.remove('modal-open');
    if (opener) opener.focus({ preventScroll: true });
  });
})();
