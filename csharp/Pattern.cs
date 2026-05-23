// Access pattern detector (Adaptive-prefetch-spec §4).

using System.Collections.Generic;

namespace Nxs;

public enum AccessPattern
{
    Unknown,
    Sequential,
    Random,
    Mixed,
}

public static class PatternConstants
{
    public const int SequentialThreshold = 10;
    public const int RandomThreshold = 100;
    public const int HistorySize = 32;
    public const int MinObservations = 8;
    public const int UpgradeSequentialThreshold = 100;
}

/// <summary>Observes record indices and classifies access patterns.</summary>
public sealed class AccessPatternDetector
{
    private readonly long[] _accesses = new long[PatternConstants.HistorySize];
    private int _writePos;
    private int _filled;
    private uint _sequentialRuns;
    private uint _randomJumps;
    private long _lastIndex = -1;

    public AccessPatternDetector()
    {
        for (int i = 0; i < _accesses.Length; i++)
            _accesses[i] = -1;
    }

    public uint SequentialRuns => _sequentialRuns;

    public long LastIndex => _lastIndex;

    public void Observe(int index)
    {
        long idx = index;
        if (_lastIndex >= 0)
        {
            ulong delta = idx >= _lastIndex
                ? (ulong)(idx - _lastIndex)
                : (ulong)(_lastIndex - idx);
            if (delta <= PatternConstants.SequentialThreshold)
            {
                if (_sequentialRuns < uint.MaxValue)
                    _sequentialRuns++;
            }
            else if (delta > PatternConstants.RandomThreshold)
            {
                if (_randomJumps < uint.MaxValue)
                    _randomJumps++;
            }
        }
        _accesses[_writePos] = idx;
        _writePos = (_writePos + 1) % PatternConstants.HistorySize;
        if (_filled < PatternConstants.HistorySize)
            _filled++;
        _lastIndex = idx;
    }

    public AccessPattern Pattern()
    {
        int total = (int)_sequentialRuns + (int)_randomJumps;
        if (total < PatternConstants.MinObservations)
            return AccessPattern.Unknown;
        if (_sequentialRuns > _randomJumps * 3)
            return AccessPattern.Sequential;
        if (_randomJumps > _sequentialRuns)
            return AccessPattern.Random;
        return AccessPattern.Mixed;
    }

    public static string PatternName(AccessPattern p) => p switch
    {
        AccessPattern.Sequential => "sequential",
        AccessPattern.Random => "random",
        AccessPattern.Mixed => "mixed",
        _ => "unknown",
    };

    /// <summary>Predicted next record indices when pattern is sequential (§4.4).</summary>
    public List<int> PredictNext(int depth, int recordCount)
    {
        if (Pattern() != AccessPattern.Sequential || _lastIndex < 0)
            return new List<int>();
        int start = (int)_lastIndex + 1;
        var outList = new List<int>(depth);
        for (int i = 0; i < depth; i++)
        {
            int idx = start + i;
            if (idx < recordCount)
                outList.Add(idx);
        }
        return outList;
    }
}
