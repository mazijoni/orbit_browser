// Like many extensions, opens its welcome page when installed.
chrome.runtime.onInstalled.addListener(() => {
  chrome.tabs.create({url: chrome.runtime.getURL('welcome.html')});
});
