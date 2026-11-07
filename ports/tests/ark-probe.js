let sum = 0;
for (let i = 1; i <= 1000; i++) sum += i;
if (sum !== 500500) throw new Error('sum mismatch');
globalThis.__auroraArkProbe = sum;
