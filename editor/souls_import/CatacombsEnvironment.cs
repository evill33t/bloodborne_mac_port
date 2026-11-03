// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Globalization;

// Catacombs has short, dark interior haze and a separate almost-clear outdoor
// row. The Firelink calibration intentionally uses a common distant horizon;
// applying that same distance to every m13 row erases this distinction.
internal static class CatacombsEnvironment {
    static string Leaf(string name)=>Path.GetFileName(name.Replace('\\','/'));
    static float Number(PARAM.Row row,string name) {
        var value=Convert.ToSingle(row.Cells.Single(c=>c.Def.InternalName==name).Value,CultureInfo.InvariantCulture);
        if(!float.IsFinite(value))throw new InvalidDataException("Nonfinite Catacombs fog field: "+name);return value;
    }
    internal static void Apply(EnvironmentResult environment,Func<string,byte[]?> sourceRead,string definitionsDirectory) {
        if(!environment.Calibration.SourceFog)return;
        var data=sourceRead("param/DrawParam/a13_DrawParam.parambnd.dcx")??throw new FileNotFoundException("Missing Catacombs draw parameters");
        var file=BND3.Read(data).Files.Single(f=>Leaf(f.Name)=="m13_FogBank.param");var fog=PARAM.Read(file.Bytes);
        fog.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.Combine(definitionsDirectory,"FogBank.xml")));
        var rows=environment.Rows.Where(r=>r.Kind=="Fog"&&r.SourceID!=255&&r.NativeID.HasValue).ToArray();
        var conversions=rows.ToDictionary(r=>r.NativeID!.Value,r=> {
            var source=fog.Rows.Single(p=>p.ID==r.SourceID);float begin=Number(source,"fogBeginZ"),end=Number(source,"fogEndZ");
            if(begin is <-100000 or >100000||end<=begin||end>100000)throw new InvalidDataException("Unsupported Catacombs fog interval");
            // Native exponential fog reaches half opacity after one Fog-Dist.
            // Match the source linear curve's midpoint; retain negative starts,
            // which are also authored by retail BB profiles (down to -12m).
            return (Begin:begin,Distance:(end-begin)*.5f);
        });
        byte[] Patch(byte[] bytes) {
            var g=GPARAM.Read(bytes);var group=g.Params.Single(p=>p.Key=="FogParamEditor");
            void Set(string key,int id,float value)=>((GPARAM.FloatField)group.Fields.Single(f=>f.Key==key)).Values.Single(v=>v.ID==id).Value=value;
            foreach(var (id,curve) in conversions){Set("Fog-DistOffset",id,curve.Begin);Set("Fog-Dist",id,curve.Distance);Set("Fog-fogAlpha",id,1);Set("Fog-heightFogDensity",id,0);}
            return g.Write(DCX.Type.DCX_DFLT_10000_44_9);
        }
        for(int i=0;i<environment.Files.Count;i++) {
            var input=environment.Files[i];byte[] result;
            if(input.Path.EndsWith(".gparambnd.dcx",StringComparison.Ordinal)) {
                var binder=BND4.Read(input.Bytes);foreach(var entry in binder.Files)entry.Bytes=DCX.Decompress(Patch(entry.Bytes.ToArray()));result=binder.Write(DCX.Type.DCX_DFLT_10000_44_9);
            } else if(input.Path.EndsWith(".gparam.dcx",StringComparison.Ordinal))result=Patch(input.Bytes);
            else throw new InvalidDataException("Unexpected Catacombs environment file: "+input.Path);
            environment.Files[i]=input with{Bytes=result};
        }
        foreach(var row in rows) {
            var curve=conversions[row.NativeID!.Value];var values=new Dictionary<string,object>(row.Values){["calibrationStart"]=curve.Begin,["calibrationDistance"]=curve.Distance,["calibrationStrength"]=1f,["curveMatch"]="source linear midpoint"};
            environment.Rows[environment.Rows.IndexOf(row)]=row with{Values=values,Status="Catacombs source-specific fog interval: native exponential haze matches the source linear midpoint with source color calibration retained. Approximation; end-distance opacity and height response differ."};
        }
        environment.Limitations.Add("Catacombs fog now uses each source zone's near/far interval instead of Firelink's shared horizon distance. Native exponential falloff remains an approximation of DS1R's linear curve.");
    }
}
