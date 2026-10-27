// Proves the popup page runs as an extension page: chrome.tabs answers with
// the tab the popup was opened for, and a script can run in it.
chrome.tabs.query({active: true, currentWindow: true}).then(async (tabs) => {
  document.getElementById('who').textContent = tabs.length + ' tab(s)';
  let page = '';
  try {
    const [r] = await chrome.scripting.executeScript({target: {tabId: tabs[0].id}, func: () => document.title});
    page = r.result;
  } catch (e) {
    page = 'ERR ' + e.message;
  }
  document.title = 'POPUP:' + tabs.length + ':' + page;
}, (e) => { document.title = 'POPUP-ERR:' + e; });
