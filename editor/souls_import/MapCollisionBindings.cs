// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;

// Keep H/L chunks associated with the same source placement, so lighting and
// area metadata still follow source collision regions after native partitioning.
internal static class MapCollisionBindings {
    internal record Chunk(string SourceName,IReadOnlyList<MapCollision.Triangle> High,IReadOnlyList<MapCollision.Triangle> Low);
    internal static IReadOnlyList<Chunk> Partition(SourceCollision.Result source,int slots) {
        var high=source.Parts.Where(p=>p.Layer=="h"&&p.PlayerSolid).ToDictionary(p=>p.Name,StringComparer.Ordinal);
        var low=source.Parts.Where(p=>p.Layer=="l"&&p.PlayerSolid).ToDictionary(p=>p.Name,StringComparer.Ordinal);
        if(high.Count==0||slots<high.Count||!high.Keys.Order().SequenceEqual(low.Keys.Order()))
            throw new InvalidDataException("Native collision slots must accommodate paired source H/L placements");
        var names=high.Keys.Order(StringComparer.Ordinal).ToArray();
        if(names.Any(n=>high[n].TriangleCount==0||low[n].TriangleCount==0))throw new InvalidDataException("Empty source solid collision placement");
        var counts=names.ToDictionary(n=>n,_=>1,StringComparer.Ordinal);
        for(int i=names.Length;i<slots;i++) {
            var name=names.Where(n=>counts[n]<Math.Min(high[n].TriangleCount,low[n].TriangleCount))
                .MaxBy(n=>(double)(high[n].TriangleCount+low[n].TriangleCount)/(counts[n]+1))
                ??throw new InvalidDataException("Too many native slots for source collision triangles");
            counts[name]++;
        }
        var chunks=new List<Chunk>();
        foreach(string name in names) {
            var h=MapCollision.Partition(high[name].Surfaces.SelectMany(s=>s.Triangles).ToList(),counts[name]);
            var l=MapCollision.Partition(low[name].Surfaces.SelectMany(s=>s.Triangles).ToList(),counts[name]);
            for(int i=0;i<counts[name];i++)chunks.Add(new(name,h[i],l[i]));
        }
        return chunks;
    }
}
