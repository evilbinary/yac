let app = document.getElementById('app');
let d = document.createElement('div');
d.textContent = 'DOM bridge OK';
app.appendChild(d);
let p = document.createElement('p');
p.textContent = 'rendered by yui';
app.appendChild(p);
yuiSpinFrames(180);
