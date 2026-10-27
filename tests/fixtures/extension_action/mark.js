// Marks the page; a click is a user gesture (Picture-in-Picture needs one).
document.title = 'ACTION-CLICKED:' + (navigator.userActivation ? navigator.userActivation.isActive : 'none');
