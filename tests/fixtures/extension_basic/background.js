// Like many extensions, opens pages when installed: a tab, and a popup window.
chrome.runtime.onInstalled.addListener(() => {
  chrome.tabs.create({url: chrome.runtime.getURL('welcome.html')});
  chrome.windows.create({url: chrome.runtime.getURL('welcome.html?window'), type: 'popup', width: 320, height: 240});
});
