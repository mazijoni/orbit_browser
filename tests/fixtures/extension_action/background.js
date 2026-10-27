// The toolbar button was clicked: run mark.js in the page (activeTab grants
// access to it), as Picture-in-Picture runs its script.
chrome.action.onClicked.addListener((tab) => {
  chrome.scripting.executeScript({target: {tabId: tab.id, allFrames: true}, files: ['mark.js']});
});
