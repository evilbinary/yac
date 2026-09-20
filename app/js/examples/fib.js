// 递归 / 数组 / 字符串 / 模板串，覆盖 lang.js 的常用子集。
function fib(n) {
  if (n <= 1) { return n; }
  return fib(n - 1) + fib(n - 2);
}

for (let i = 0; i < 10; i++) {
  console.log(`fib(${i}) = ${fib(i)}`);
}

let xs = [3, 1, 2];
xs.sort(function (a, b) { return a - b; });
console.log("sorted:", xs.join(","));

let o = { name: "yac", n: 2 };
console.log(`${o.name} x ${o.n} = ${o.n * 2}`);
