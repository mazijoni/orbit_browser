// Proves the popup page runs as an extension page: chrome.tabs answers.
chrome.tabs.query({active: true, currentWindow: true}).then((tabs) => {
  document.getElementById('who').textContent = tabs.length + ' tab(s)';
  document.title = 'POPUP:' + tabs.length;
}, (e) => { document.title = 'POPUP-ERR:' + e; });
