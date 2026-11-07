// The Wine Mono rung of tests/ladder (LADDER_HEAVY=mono). Prints the same LADDER lines as the
// C++ programs. Exercises what Mono needs from Wine and FEX: its JIT writing and running code,
// hardware faults turned into managed exceptions (null dereference, integer divide by zero),
// threads and monitors, P/Invoke into kernel32, and the garbage collector.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;

static class Ladder
{
    static int pass, fail;

    static void Line(string status, string check, string detail)
    {
        Console.WriteLine("LADDER\t" + status + "\tmono." + check + "\t" + detail.Replace('\t', ' ').Replace('\n', ' '));
    }

    static void Check(bool ok, string check, string detail)
    {
        if (ok) pass++; else fail++;
        Line(ok ? "PASS" : "FAIL", check, detail);
    }

    [DllImport("kernel32.dll")]
    static extern uint GetCurrentProcessId();

    static int Divide(int a, int b) { return a / b; }

    static long Checksum<T>(IEnumerable<T> items, Func<T, long> f)
    {
        long s = 0;
        foreach (T x in items) s = (s * 31 + f(x)) % 1000000007L;
        return s;
    }

    static int Main()
    {
        Line("INFO", "env", "CLR " + Environment.Version + ", 64-bit process " + Environment.Is64BitProcess + ", " + Environment.OSVersion);
        Check(true, "hello", "hello from C#");

        string caught = "none";
        try { throw new InvalidOperationException("ladder"); }
        catch (InvalidOperationException e) { caught = e.Message; }
        Check(caught == "ladder", "throw_catch", "managed throw/catch got '" + caught + "'");

        caught = "none";
        try { object o = null; caught = o.ToString(); }
        catch (NullReferenceException) { caught = "NullReferenceException"; }
        Check(caught == "NullReferenceException", "null_reference", "null dereference became " + caught);

        caught = "none";
        try { Divide(7, 0); }
        catch (DivideByZeroException) { caught = "DivideByZeroException"; }
        Check(caught == "DivideByZeroException", "divide_by_zero", "integer divide by zero became " + caught);

        int counter = 0, locked = 0;
        object gate = new object();
        var threads = new List<Thread>();
        for (int t = 0; t < 4; t++)
        {
            var th = new Thread(() =>
            {
                for (int i = 0; i < 100000; i++)
                {
                    Interlocked.Increment(ref counter);
                    lock (gate) locked++;
                }
            });
            threads.Add(th);
            th.Start();
        }
        foreach (var th in threads) th.Join();
        Check(counter == 400000 && locked == 400000, "threads", "Interlocked " + counter + ", lock " + locked + " (want 400000 each)");

        try
        {
            uint pid = GetCurrentProcessId();
            Check(pid == (uint)Process.GetCurrentProcess().Id, "pinvoke", "kernel32 GetCurrentProcessId " + pid);
        }
        catch (Exception e)
        {
            Check(false, "pinvoke", "kernel32 GetCurrentProcessId threw " + e.GetType().Name);
        }

        var keep = new List<int[]>();
        for (int i = 0; i < 20000; i++)
        {
            var a = new int[2500];   // ~200 MB churned in total
            a[0] = i;
            if (i % 100 == 0) keep.Add(a);
        }
        GC.Collect();
        GC.WaitForPendingFinalizers();
        bool kept = keep.Count == 200;
        for (int i = 0; i < keep.Count && kept; i++) kept = keep[i][0] == i * 100;
        Check(kept, "gc", "200 survivors intact after ~200 MB of garbage and a full collection");

        var nums = new List<int>();
        for (int i = 0; i < 100000; i++) nums.Add((i * 7919) % 100003);
        nums.Sort();
        long sum = Checksum(nums, x => (long)x * x);
        long sum2 = Checksum(new[] { "a", "bb", "ccc" }, s => s.Length);
        Check(sum == ExpectedSum() && sum2 == 1026, "jit_generics", "generic checksums " + sum + ", " + sum2);

        Console.WriteLine("LADDER\tDONE\tmono\tpass=" + pass + " fail=" + fail + " skip=0 info=1");
        return fail == 0 ? 0 : 1;
    }

    // The same checksum computed without generics or lambdas.
    static long ExpectedSum()
    {
        var a = new int[100000];
        for (int i = 0; i < a.Length; i++) a[i] = (i * 7919) % 100003;
        Array.Sort(a);
        long s = 0;
        for (int i = 0; i < a.Length; i++) s = (s * 31 + (long)a[i] * a[i]) % 1000000007L;
        return s;
    }
}
