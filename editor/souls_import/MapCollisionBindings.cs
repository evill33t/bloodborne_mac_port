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
            Split(name,high[name].Surfaces.SelectMany(s=>s.Triangles).ToList(),low[name].Surfaces.SelectMany(s=>s.Triangles).ToList(),counts[name]);
        }
        return chunks;

        // A native collision record owns both layers. Independent medians can
        // pair a fine stair section with a coarse section on another floor.
        // Use one world-space cutting plane for the two layers at every split.
        void Split(string name,List<MapCollision.Triangle> h,List<MapCollision.Triangle> l,int count) {
            if(count==1){chunks.Add(new(name,h,l));return;}
            int left=count/2,right=count-left;
            float Key(MapCollision.Triangle t,int axis){var p=(t.A+t.B+t.C)/3;return axis==0?p.X:axis==1?p.Y:p.Z;}
            (int axis,float cut,float score)? best=null;
            for(int axis=0;axis<3;axis++) {
                var hs=h.Select(t=>Key(t,axis)).Order().ToArray();var ls=l.Select(t=>Key(t,axis)).Order().ToArray();
                float lo=Math.Max(hs[left-1],ls[left-1]),hi=Math.Min(hs[^right],ls[^right]);
                if(lo>=hi)continue;
                float desired=(hs[(int)((long)hs.Length*left/count)]+ls[(int)((long)ls.Length*left/count)])*.5f;
                float cut=Math.Clamp(desired,MathF.BitIncrement(lo),hi);
                int nh=hs.Count(v=>v<cut),nl=ls.Count(v=>v<cut);
                if(nh<left||nl<left||hs.Length-nh<right||ls.Length-nl<right)continue;
                float score=Math.Abs((float)nh/hs.Length-(float)left/count)+Math.Abs((float)nl/ls.Length-(float)left/count);
                if(best==null||score<best.Value.score)best=(axis,cut,score);
            }
            if(best is not {} plane)throw new InvalidDataException("Source H/L collision cannot share the requested spatial partitions: "+name);
            Split(name,h.Where(t=>Key(t,plane.axis)<plane.cut).ToList(),l.Where(t=>Key(t,plane.axis)<plane.cut).ToList(),left);
            Split(name,h.Where(t=>Key(t,plane.axis)>=plane.cut).ToList(),l.Where(t=>Key(t,plane.axis)>=plane.cut).ToList(),right);
        }
    }
}
