/**
 * Fire TV Remote and D-pad Key Navigation Helper for HamClock Vega
 */

export interface RemoteHandlerCallbacks {
  onOpenSettings: () => void;
  onBack: () => void;
}

export function generateRemoteInjectionScript(): string {
  return `
    (function() {
      // Ensure virtual cursor is visible and initialized
      if (typeof initVirtualCursor === 'function') {
        initVirtualCursor();
      }

      window.addEventListener('keydown', function(e) {
        switch(e.key) {
          case 'ArrowUp':
            if (typeof handleVirtualCursorMove === 'function') {
              handleVirtualCursorMove('ArrowUp');
              e.preventDefault();
            }
            break;
          case 'ArrowDown':
            if (typeof handleVirtualCursorMove === 'function') {
              handleVirtualCursorMove('ArrowDown');
              e.preventDefault();
            }
            break;
          case 'ArrowLeft':
            if (typeof handleVirtualCursorMove === 'function') {
              handleVirtualCursorMove('ArrowLeft');
              e.preventDefault();
            }
            break;
          case 'ArrowRight':
            if (typeof handleVirtualCursorMove === 'function') {
              handleVirtualCursorMove('ArrowRight');
              e.preventDefault();
            }
            break;
          case 'Enter':
          case 'Select':
            if (typeof handleVirtualCursorClick === 'function') {
              handleVirtualCursorClick();
              e.preventDefault();
            }
            break;
        }
      }, true);
    })();
  `;
}
