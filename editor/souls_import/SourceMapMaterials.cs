// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Buffers.Binary;

// Scoped to m12/m13 imports by the caller. Preserve authored surface gains
// with private definitions; crawling overlay semantics remain Catacombs-only.
internal sealed class SourceMapMaterials(Func<string,MTD> sourceDefinition,Func<string,MTD> nativeDefinition,string sourceMap="m13_00_00_00") {
    readonly Dictionary<(string Source,string Native),EnvironmentMaterial> converted=[];
    internal IEnumerable<EnvironmentMaterial> Definitions=>converted.Values;
    static string Leaf(string path)=>Path.GetFileName(path.Replace('\\','/'));
    internal static bool Crawling(string path)=>Leaf(path).Equals("A13_musi_02[DSB]_edge_.mtd",StringComparison.OrdinalIgnoreCase);
    internal sealed record Crawl(Vector2 DiffusePerSecond,Vector2 BumpPerSecond,Vector2 DetailPerSecond);
    internal static Crawl CrawlingMotion(MTD source) {
        object Value(string name)=>source.Params.Single(p=>p.Name==name).Value;
        if((int)Value("g_TexScrollType")!=1||(int)Value("g_BlendMode")!=1)throw new InvalidDataException("Unsupported Catacombs crawling material mode");
        Vector2 Speed(string name){var p=(float[])Value(name);if(p.Length!=2||p.Any(v=>!float.IsFinite(v)||Math.Abs(v)>4))throw new InvalidDataException("Invalid crawling material speed");return new(p[0],p[1]);}
        return new(Speed("g_TexScroll_0"),Speed("g_TexScroll_1"),Speed("g_TexScroll_2"));
    }
    internal void ApplyCrawling(IFlverMaterial source,FLVER2 model,FLVER2.Material material) {
        if(!Crawling(source.MTD))return;
        if(model.Header.BigEndian||model.Header.Version is not(0x20013 or 0x20014))throw new InvalidDataException("Crawling animation requires a native BB model");
        var motion=CrawlingMotion(sourceDefinition(source.MTD));
        FLVER2.GXList list;
        if(material.GXIndex==-1)list=new();
        else {
            if(material.GXIndex<0||material.GXIndex>=model.GXLists.Count)throw new InvalidDataException("Invalid native crawling material GX index");
            var old=model.GXLists[material.GXIndex];list=new(){TerminatorID=old.TerminatorID,TerminatorLength=old.TerminatorLength};
            foreach(var item in old)list.Add(new(item.ID,item.Unk04,item.Data.ToArray()));
        }
        void Scroll(string id,Vector2 rate) {
            var items=list.Where(i=>i.ID==id).ToArray();if(items.Length>1||items.Any(i=>i.Unk04!=100||i.Data.Length!=12))throw new InvalidDataException("Unsupported native texture-scroll GX record");
            var item=items.SingleOrDefault();
            if(item==null){item=new(id,100,new byte[12]);list.Add(item);}
            BinaryPrimitives.WriteSingleLittleEndian(item.Data.AsSpan(0,4),rate.X);BinaryPrimitives.WriteSingleLittleEndian(item.Data.AsSpan(4,4),rate.Y);
        }
        // BB e57c40 dispatches GX20 as texture0/channel0 and GX26 as
        // texture6/channel3; edb9e0 consumes the first two float words, leaving
        // the third word untouched. The native GX vertex shader already reads
        // cbTexScroll. Never mutate the donor list shared by other materials.
        Scroll("GX20",motion.DiffusePerSecond);Scroll("GX26",motion.BumpPerSecond);
        material.GXIndex=model.GXLists.Count;model.GXLists.Add(list);
    }
    internal void Convert(IFlverMaterial source,FLVER2.Material target) {
        // Placed loot bodies also pass through static baking. Their armor MTDs
        // use character material semantics (and the retail binder contains two
        // distinct P_DullLeather definitions). Keep this environment treatment
        // on map surfaces instead of choosing an arbitrary character definition.
        if(Leaf(source.MTD).StartsWith("P_",StringComparison.OrdinalIgnoreCase))return;
        string native=Leaf(target.MTD);
        if(native is not("M[ARSN].mtd" or "M[ARSN]_e.mtd"))return;
        var key=(source.MTD,target.MTD);
        if(!converted.TryGetValue(key,out var result)) {
            var from=sourceDefinition(source.MTD);var color=from.Params.SingleOrDefault(p=>p.Name=="g_DiffuseMapColor")?.Value as float[];
            var gain=from.Params.SingleOrDefault(p=>p.Name=="g_DiffuseMapColorPower")?.Value;
            if(color==null||gain is not float power)return;
            if(color.Length!=3||color.Any(v=>!float.IsFinite(v)||v<0||v>4)||!float.IsFinite(power)||power<0||power>4)throw new InvalidDataException("Unsupported Catacombs diffuse material gain");
            if(Crawling(source.MTD))_ = CrawlingMotion(from);
            var definition=MTD.Read(nativeDefinition(target.MTD).Write());
            // Native GX has a diffuse RGB multiplier, whereas the source splits
            // tint and power. Preserve the authored product rather than dropping
            // the .6 gain on most Catacombs stone. BRDF/color-space equivalence
            // remains a visual calibration, independent of the light-bank port.
            definition.Params.Single(p=>p.Name=="g_DiffuseMapColor").Value=color.Select(v=>v*power).ToArray();
            string name="BBImport_"+sourceMap[..3]+"_material_"+converted.Count.ToString("D3")+".mtd";
            result=new(name,definition);converted.Add(key,result);
        }
        target.MTD=@"N:\SPRJ\data\Material\mtd\map\BBImport\"+result.Name;
    }
}
