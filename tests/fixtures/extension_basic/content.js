// Marks the page so the self-test can see that a content script ran, and that
// the extension APIs exist in its world.
document.title = 'EXT:' + document.title + ':' + (chrome.runtime && chrome.runtime.id ? 'api' : 'noapi');
