// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Globalization;
using System.Numerics;
using System.Text.RegularExpressions;

internal record EnvironmentBinding(string Name,int LightID,int FogID,int ScatterID,int ShadowID=0,bool BindScene=false,int ToneMapID=0);
internal record EnvironmentCalibration(bool SourceFog=true,float ToneEV=-.4f,float FogStrength=.30f,float FogDistance=80f,float FogStart=25f,float FogColorScale=.35f);
internal record EnvironmentSceneBinding(int ToneMapID);
internal record EnvironmentFile(string Path,byte[] Bytes);
internal record EnvironmentRow(string Kind,int SourceID,int? NativeID,Dictionary<string,object> Values,string Status);
internal record EnvironmentResult(Dictionary<string,MSBB.Part.GparamConfigStruct> Bindings,List<EnvironmentFile> Files,List<EnvironmentRow> Rows,List<string> Limitations) {
    public Dictionary<string,int> ShadowBindings { get; init; }=new(StringComparer.Ordinal);
    public Dictionary<string,EnvironmentSceneBinding> SceneBindings { get; init; }=new(StringComparer.Ordinal);
    public EnvironmentCalibration Calibration { get; init; }=new();
}
internal record EnvironmentMaterial(string Name,MTD Definition);
internal record EnvironmentSky(FLVER2 Model,List<EnvironmentMaterial> Materials,List<string> Limitations);

internal static class MapEnvironment {
    const string LightKey="LightSet ParamEditor",ScatterKey="LightScattering : ParamEditor",FogKey="FogParamEditor",ShadowKey="ShadowParam ParamEditor",ShadowAngle="hadowParam - Shadow: Angle0",ToneKey="Tone Map[Yebis]",ExposureKey="Yebis-ToneMapExposure";
    static string Leaf(string path)=>Path.GetFileName(path.Replace('\\','/'));
    static byte[] Required(Func<string,byte[]?> read,string path)=>read(path)??throw new FileNotFoundException("Map environment asset unavailable",path);
    static PARAM.Row Row(PARAM p,int id)=>p.Rows.SingleOrDefault(r=>r.ID==id)??throw new InvalidDataException($"Missing {p.ParamType} row {id}");
    static object Cell(PARAM.Row r,string key)=>r.Cells.SingleOrDefault(c=>c.Def.InternalName==key)?.Value??throw new InvalidDataException("Missing environment field: "+key);
    static float Number(PARAM.Row r,string key){var value=System.Convert.ToSingle(Cell(r,key),CultureInfo.InvariantCulture);if(!float.IsFinite(value))throw new InvalidDataException("Nonfinite environment field: "+key);return value;}
    static short Short(PARAM.Row r,string key)=>checked((short)System.Convert.ToInt32(Cell(r,key),CultureInfo.InvariantCulture));
    // DS1R 0x140564630 and BB 0x022e0860 both form
    // (sin(yaw)*cos(pitch), -sin(pitch), cos(yaw)*cos(pitch)); preserve signs.
    static Vector2 Angle(PARAM.Row row,string suffix)=>new(Number(row,"degRotX_"+suffix)*MathF.PI/180,Number(row,"degRotY_"+suffix)*MathF.PI/180);
    static Vector4 Color(PARAM.Row row,string suffix,string sourceProfile) {
        var color=new Vector4(Number(row,"colR_"+suffix)/255,Number(row,"colG_"+suffix)/255,Number(row,"colB_"+suffix)/255,Number(row,"colA_"+suffix)/100);
        if(sourceProfile!="DS1R")return color;
        // Shipped DSR exe (SHA256 a45aaa36dd2f6cc151670a639ea5547043cf38ea79ff4178b963c6ed71f98d7b):
        // 0x140564510 calls imported powf(abs(RGB/255 * A/100), 2.2), with
        // exponent at 0x1412aa960. BB eboot SHA256 cec1b276e7f9e4db978e57f524f41fbaac594530a3437b002e23f3fab14b4f86:
        // 0x022de8b0 and 0x022e0b03 consume RGB*RGB*W, including hemisphere.
        // Factor the source energy across native RGB and W without clipping HDR.
        color=new(MathF.Pow(MathF.Abs(color.X),1.1f),MathF.Pow(MathF.Abs(color.Y),1.1f),MathF.Pow(MathF.Abs(color.Z),1.1f),MathF.Pow(MathF.Abs(color.W),2.2f));
        if(!float.IsFinite(color.X)||!float.IsFinite(color.Y)||!float.IsFinite(color.Z)||!float.IsFinite(color.W))throw new InvalidDataException("DS1R light color exceeds the native numeric range");
        return color;
    }
    static Dictionary<string,object> Values(PARAM.Row row)=>row.Cells.Where(c=>!c.Def.InternalName.StartsWith("pad",StringComparison.Ordinal)).ToDictionary(c=>c.Def.InternalName,c=>c.Value);
    static GPARAM.Param Group(GPARAM g,string key)=>g.Params.SingleOrDefault(p=>p.Key==key)??throw new InvalidDataException("Missing native environment group: "+key);
    static GPARAM.Field<T> Field<T>(GPARAM.Param group,string key)=>group.Fields.SingleOrDefault(f=>f.Key==key) as GPARAM.Field<T>??throw new InvalidDataException("Missing or incompatible native environment field: "+key);
    static void Set<T>(GPARAM.Param group,string key,int id,T value){var f=Field<T>(group,key);f.Values.Single(v=>v.ID==id).Value=value;}
    static void Append<T>(GPARAM.Field<T> field,int template,int id) {
        if(field.Values.Count>=255||field.Values.Any(v=>v.ID==id))throw new InvalidDataException("Native GPARAM field exceeds its row budget or repeats a row ID");
        var source=field.Values.SingleOrDefault(v=>v.ID==template)??throw new InvalidDataException("Native GPARAM template row is missing");
        field.Values.Add(new(){ID=id,TimeOfDay=source.TimeOfDay,Value=source.Value});field.Capacity=(short)field.Values.Count;
    }
    static GPARAM.Param Append(GPARAM g,string key,int id) {
        var group=Group(g,key);
        foreach(var field in group.Fields)switch(field) {
            case GPARAM.ShortField f:Append(f,0,id);break;case GPARAM.IntField f:Append(f,0,id);break;case GPARAM.UintField f:Append(f,0,id);break;
            case GPARAM.FloatField f:Append(f,0,id);break;case GPARAM.BoolField f:Append(f,0,id);break;
            case GPARAM.Vector2Field f:Append(f,0,id);break;case GPARAM.Vector3Field f:Append(f,0,id);break;case GPARAM.Vector4Field f:Append(f,0,id);break;
            case GPARAM.ColorField f:Append(f,0,id);break;
            default:throw new InvalidDataException("Unsupported native GPARAM environment field type: "+field.GetType().Name);
        }
        // BB 0x0218f870 / 0x0218f8b0 / 0x0218f8f0 enumerate groups and row
        // IDs through the separate GPARAM index tables. Field values alone do
        // not register an added row. Retain native historical IDs and ordering.
        int index=g.Params.IndexOf(group);var entry=g.UnkParamExtras.SingleOrDefault(e=>e.GroupIndex==index);
        if(entry==null) {
            entry=new(){GroupIndex=index};int at=g.UnkParamExtras.FindIndex(e=>e.GroupIndex>index);
            if(at<0)g.UnkParamExtras.Add(entry);else g.UnkParamExtras.Insert(at,entry);
        }
        if(entry.Ids.Contains(id))throw new InvalidDataException("Native GPARAM group already reserves the new row ID");
        entry.Ids.Add(id);return group;
    }
    static void Light(GPARAM.Param group,int id,PARAM.Row row,string sourceProfile) {
        // DS1 has three independently directed diffuse lights and a fourth
        // specular light. Preserve each direction in its own native slot.
        Set(group,"Directional Light Angle0",id,Angle(row,"0"));Set(group,"Directional Light DiffColor0",id,Color(row,"0",sourceProfile));Set(group,"Directional Light SpecColor0",id,Vector4.Zero);
        Set(group,"Directional Light Angle1",id,Angle(row,"1"));Set(group,"Directional Light DiffColor1",id,Color(row,"1",sourceProfile));Set(group,"Directional Light SpecColor1",id,Vector4.Zero);
        Set(group,"Directional Light LocalLight Angle1",id,Angle(row,"2"));Set(group,"Directional Light LocalLight DiffColor1",id,Color(row,"2",sourceProfile));Set(group,"Directional Light LocalLight SpecColor1",id,Vector4.Zero);
        Set(group,"Directional Light LocalLight Angle2",id,Angle(row,"s"));Set(group,"Directional Light LocalLight DiffColor2",id,Vector4.Zero);Set(group,"Directional Light LocalLight SpecColor2",id,Color(row,"s",sourceProfile));
        Set(group,"Directional Light Hemi Color Up",id,Color(row,"u",sourceProfile));Set(group,"Directional Light Hemi Color Down",id,Color(row,"d",sourceProfile));
        Set(group,"Directional LightLight1 FollowedCamera",id,false);Set(group,"Directional Enable LocalLight1",id,true);Set(group,"Directional Enable LocalLight2",id,true);
    }
    static void Scattering(GPARAM.Param group,int id,PARAM.Row? row) {
        var names=new[]{"Ls-sunRotX","Ls-sunRotY","Ls-距離倍率[%]","Ls-光源色R","Ls-光源色G","Ls-光源色B","Ls-光源RGB倍率[%]","Ls-散乱方向係数","Ls-レイリー散乱係数","Ls-ミー散乱係数","Ls-ブレンド係数[%]","Ls-光源色R","Ls-光源色G","Ls-光源色B","Ls-地上の乱反射光RGB倍率[%]"};
        var source=new[]{"sunRotX","sunRotY","distanceMul","sunR","sunG","sunB","sunA","lsHGg","lsBetaRay","lsBetaMie","blendCoef","reflectanceR","reflectanceG","reflectanceB","reflectanceA"};
        if(group.Fields.Count!=names.Length)throw new InvalidDataException("Unknown native scattering schema");
        // Sun RGB and ground-reflectance RGB repeat the same keys in Bloodborne.
        // Verify the complete ordered schema instead of selecting duplicate keys.
        for(int n=0;n<names.Length;n++) {
            var field=group.Fields[n];if(field.Key!=names[n])throw new InvalidDataException("Unexpected native scattering field order");
            if(n is 7 or 8 or 9) {
                if(field is not GPARAM.FloatField f)throw new InvalidDataException("Unexpected native scattering float type");
                f.Values.Single(v=>v.ID==id).Value=row==null?0:Number(row,source[n]);
            } else {
                if(field is not GPARAM.ShortField f)throw new InvalidDataException("Unexpected native scattering integer type");
                f.Values.Single(v=>v.ID==id).Value=row==null?(short)(n is 2 or 14?100:0):Short(row,source[n]);
            }
        }
    }
    static void Fog(GPARAM.Param group,int id,PARAM.Row row,EnvironmentCalibration calibration) {
        // Native 0x022d6720 squares fog RGB. Preserve source hue in linear
        // space, with an explicit artistic gain; do not apply LightBank gamma.
        float intensity=Number(row,"colA")/100*calibration.FogColorScale;
        var rgb=new Vector3(Number(row,"colR")/255,Number(row,"colG")/255,Number(row,"colB")/255)*intensity;
        if(rgb.X<0||rgb.Y<0||rgb.Z<0||!float.IsFinite(rgb.LengthSquared()))throw new InvalidDataException("Unsupported source fog color range");
        Set(group,"Fog-Color",id,new Vector4(MathF.Sqrt(rgb.X),MathF.Sqrt(rgb.Y),MathF.Sqrt(rgb.Z),1));
        Set(group,"Fog-fogAlpha",id,calibration.FogStrength);Set(group,"Fog-Dist",id,calibration.FogDistance);Set(group,"Fog-DistOffset",id,calibration.FogStart);
        Set(group,"Fog-heightColor",id,Vector4.Zero);Set(group,"Fog-fogContrast",id,0f);
    }
    static void Tone(GPARAM.Param group,int id,EnvironmentCalibration calibration) {
        // Native 0x021d4f43 -> 0x021d7163 -> 0x00bb3550 -> 0x00bd4d40
        // passes positive manual exposure unchanged to Yebis member +0x700.
        // 0x00bd92f0 divides it by scene luminance: exposure is linear, not EV.
        if(Field<int>(group,"Yebis-AutoExposure").Values.Single(v=>v.ID==0).Value!=0)throw new InvalidDataException("Tone calibration requires a native manual-exposure template");
        float exposure=Field<float>(group,ExposureKey).Values.Single(v=>v.ID==0).Value*MathF.Pow(2,calibration.ToneEV);
        if(!float.IsFinite(exposure)||exposure<=0)throw new InvalidDataException("Invalid native tone exposure");
        Set(group,ExposureKey,id,exposure);
    }
    internal static EnvironmentResult Convert(PARAM light,PARAM fog,PARAM scattering,IReadOnlyDictionary<string,byte[]> profiles,IReadOnlyList<EnvironmentBinding> bindings,string sourceProfile="DS1",PARAM? shadow=null,EnvironmentCalibration? calibration=null,PARAM? toneMap=null) {
        calibration??=new();
        if(!float.IsFinite(calibration.ToneEV)||calibration.ToneEV is <-8 or >8||!float.IsFinite(calibration.FogStrength)||calibration.FogStrength is <0 or >1||!float.IsFinite(calibration.FogDistance)||calibration.FogDistance is <=0 or >100000||!float.IsFinite(calibration.FogStart)||calibration.FogStart is <0 or >100000||!float.IsFinite(calibration.FogColorScale)||calibration.FogColorScale is <0 or >16)throw new InvalidDataException("Invalid map environment calibration");
        if(sourceProfile is not("DS1" or "DS1R"))throw new InvalidDataException("No light color transfer adapter for "+sourceProfile);
        if(bindings.Select(p=>p.Name).Distinct(StringComparer.Ordinal).Count()!=bindings.Count||bindings.Any(b=>new[]{b.LightID,b.FogID,b.ScatterID,b.ShadowID,b.ToneMapID}.Any(id=>id<0||id>255)))throw new InvalidDataException("Invalid or repeated source environment binding");
        if(profiles.Count==0)throw new InvalidDataException("No native environment profiles");
        var parsed=profiles.ToDictionary(p=>p.Key,p=>GPARAM.Read(p.Value),StringComparer.Ordinal);
        if(parsed.Values.Any(g=>g.Version!=GPARAM.GparamVersion.V3))throw new InvalidDataException("Choose Bloodborne V3 environment profiles");
        if(parsed.Values.Any(g=>g.UnkParamExtras.Any(e=>e.GroupIndex<0||e.GroupIndex>=g.Params.Count)||g.UnkParamExtras.Select(e=>e.GroupIndex).Distinct().Count()!=g.UnkParamExtras.Count))throw new InvalidDataException("Invalid or repeated native GPARAM group index");
        var used=parsed.Values.SelectMany(g=>g.Params.SelectMany(p=>p.Fields).SelectMany(f=>f.Values).Select(v=>v.ID).Concat(g.UnkParamExtras.SelectMany(e=>e.Ids))).ToHashSet();int next=30000;
        int Allocate(){while(used.Contains(next))next++;used.Add(next);return next++;}
        var lights=bindings.Select(b=>b.LightID).Distinct().Order().ToDictionary(id=>id,_=>Allocate());
        var scatters=bindings.Select(b=>b.ScatterID).Distinct().Order().ToDictionary(id=>id,_=>Allocate());
        int? noFog=bindings.Any(b=>b.FogID==255)?Allocate():null;var reports=new List<EnvironmentRow>();var fogs=new Dictionary<int,int>();
        foreach(var (source,native) in lights) {
            if(source==255)throw new InvalidDataException("Disabled DS1 light-set semantics require an explicit source adapter");
            var row=Row(light,source);reports.Add(new("Light",source,native,Values(row),"Three source diffuse directions, separate specular direction and hemispheric colors mapped; "+(sourceProfile=="DS1R"?"DS1R light color energy preserves verified source pow(abs(RGB * intensity),2.2) through native squared RGB * W.":"Original DS1 retains legacy normalized color mapping; its color transfer has not been binary-verified.")));
            foreach(var g in parsed.Values)Light(Append(g,LightKey,native),native,row,sourceProfile);
        }
        foreach(var (source,native) in scatters) {
            var row=source==255?null:Row(scattering,source);reports.Add(new("Scattering",source,native,row==null?new():Values(row),row==null?"Source scattering disabled.":"Source scattering coefficients copied into corresponding native typed fields; shader equivalence requires visual validation."));
            foreach(var g in parsed.Values)Scattering(Append(g,ScatterKey,native),native,row);
        }
        foreach(int source in bindings.Select(b=>b.FogID).Distinct().Order()) {
            if(source!=255) {
                var row=Row(fog,source);var values=Values(row);
                if(!calibration.SourceFog){reports.Add(new("Fog",source,null,values,"Source fog retained as provenance; calibration disabled, preserving native row0."));continue;}
                int native=Allocate();fogs.Add(source,native);
                values["calibrationStrength"]=calibration.FogStrength;values["calibrationDistance"]=calibration.FogDistance;values["calibrationStart"]=calibration.FogStart;values["calibrationColorScale"]=calibration.FogColorScale;
                reports.Add(new("Fog",source,native,values,"Controlled native exponential horizon haze using scaled source fog hue/intensity; height fog and scene-luminance contrast disabled. Approximation, not a source linear-curve conversion."));
                foreach(var g in parsed.Values)Fog(Append(g,FogKey,native),native,row,calibration);
                continue;
            }
            reports.Add(new("Fog",source,noFog,new(),"Source fog disabled with an independent native row."));
            foreach(var g in parsed.Values){var group=Append(g,FogKey,noFog!.Value);Set(group,"Fog-fogAlpha",noFog.Value,0f);Set(group,"Fog-heightFogDensity",noFog.Value,0f);Set(group,"Fog-Color",noFog.Value,Vector4.Zero);Set(group,"Fog-heightColor",noFog.Value,Vector4.Zero);}
        }
        var shadows=new Dictionary<int,int>();
        foreach(int source in bindings.Where(b=>b.BindScene).Select(b=>b.ShadowID).Distinct().Order()) {
            if(source==255){reports.Add(new("Shadow",source,null,new(),"Disabled source ShadowID255 retained as provenance; native disabled-shadow semantics are not converted."));continue;}
            var row=Row(shadow??throw new InvalidDataException("Scene shadow bindings require a source ShadowBank"),source);int native=Allocate();shadows.Add(source,native);
            // DSR ShadowBank 0x14055b7e6 and BB ShadowParam 0x0226a110 form
            // the same (sinY*cosX, -sinX, cosY*cosX) direction as LightBank.
            var angle=new Vector2(Number(row,"lightDegRotX")*MathF.PI/180,Number(row,"lightDegRotY")*MathF.PI/180);
            foreach(var g in parsed.Values)Set(Append(g,ShadowKey,native),ShadowAngle,native,angle);
            reports.Add(new("Shadow",source,native,Values(row),"Source shadow direction converted from degrees to native radians in an independent row; native cascades, fade, bias, filter, color and density retained."));
        }
        var tones=new Dictionary<int,int>();
        if(calibration.ToneEV!=0)foreach(int source in bindings.Where(b=>b.BindScene).Select(b=>b.ToneMapID).Distinct().Order()) {
            int native=Allocate();tones.Add(source,native);var values=toneMap==null||source==255?new Dictionary<string,object>():Values(Row(toneMap,source));values["calibrationToneEV"]=calibration.ToneEV;values["calibrationExposureMultiplier"]=MathF.Pow(2,calibration.ToneEV);
            reports.Add(new("Tone",source,native,values,"Independent native manual Yebis exposure calibration relative to each phase's row0; source tone-bank fields retained as provenance only. Native gamma, curve, grading, bloom and other tone controls preserved."));
            foreach(var g in parsed.Values)Tone(Append(g,ToneKey,native),native,calibration);
        }
        var nativeBindings=bindings.ToDictionary(b=>b.Name,b=>new MSBB.Part.GparamConfigStruct{LightSetID=lights[b.LightID],FogParamID=b.FogID==255?noFog!.Value:fogs.GetValueOrDefault(b.FogID),LightScatteringID=scatters[b.ScatterID],EnvMapID=0},StringComparer.Ordinal);
        return new(nativeBindings,parsed.Select(p=>new EnvironmentFile(p.Key,p.Value.Write(DCX.Type.DCX_DFLT_10000_44_9))).ToList(),reports,
            [calibration.SourceFog?"Fog uses calibrated native exponential haze with scaled source color; DS1's linear depth curve is not reproduced.":"Source fog calibration disabled; native fog row0 retained except explicitly disabled source fog.","Source environment diffuse/specular cubemaps, point lights, shadow color/density, shadow ranges/bias/cascades, source tone curve and color grading are not converted. ToneEV adjusts native manual exposure only.","Native directional/local light slot interpretation, atmospheric scattering and material response need matched in-game validation; numeric field conversion is not a claim of identical source rendering."]) {
            ShadowBindings=bindings.Where(b=>b.BindScene&&shadows.ContainsKey(b.ShadowID)).ToDictionary(b=>b.Name,b=>shadows[b.ShadowID],StringComparer.Ordinal),
            SceneBindings=bindings.Where(b=>b.BindScene&&tones.ContainsKey(b.ToneMapID)).ToDictionary(b=>b.Name,b=>new EnvironmentSceneBinding(tones[b.ToneMapID]),StringComparer.Ordinal),Calibration=calibration
        };
    }
    internal static EnvironmentResult Load(Func<string,byte[]?> sourceRead,Func<string,byte[]?> nativeRead,string profile,string sourceMap,string targetMap,string definitionsDirectory,IReadOnlyList<EnvironmentBinding> bindings,EnvironmentCalibration? calibration=null) {
        if(profile is not("DS1" or "DS1R"))throw new NotSupportedException("No source environment adapter for "+profile);
        foreach(var map in new[]{sourceMap,targetMap})if(!Regex.IsMatch(map,@"\Am\d\d_\d\d_\d\d_\d\d\z"))throw new ArgumentException("Invalid environment map identifier");
        var sourceFiles=BND3.Read(Required(sourceRead,$"param/DrawParam/a{sourceMap.Substring(1,2)}_DrawParam.parambnd.dcx")).Files;
        PARAM Source(string kind){var p=PARAM.Read(sourceFiles.Single(f=>Leaf(f.Name)==sourceMap[..3]+"_"+kind+".param").Bytes);p.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.Combine(definitionsDirectory,kind+".xml")));return p;}
        var profiles=new Dictionary<string,byte[]>(StringComparer.Ordinal);var binders=new Dictionary<string,BND4>(StringComparer.Ordinal);
        var stem="param/drawparam/"+targetMap[..6];
        for(int phase=0;phase<4;phase++) {
            var binderPath=stem+"_"+phase.ToString("D4")+".gparambnd.dcx";var binder=BND4.Read(Required(nativeRead,binderPath));binders.Add(binderPath,binder);
            foreach(var file in binder.Files) {
                if(!Leaf(file.Name).EndsWith(".gparam",StringComparison.OrdinalIgnoreCase))throw new InvalidDataException("Unexpected native gparam binder entry");
                var path="param/drawparam/"+Leaf(file.Name)+".dcx";var loose=nativeRead(path);
                if(!profiles.TryAdd(path,loose??file.Bytes.ToArray()))throw new InvalidDataException("Repeated native environment profile: "+path);
                profiles.Add(binderPath+"::"+file.Name,file.Bytes.ToArray());
            }
        }
        var result=Convert(Source("LightBank"),Source("FogBank"),Source("LightScatteringBank"),profiles,bindings,profile,Source("ShadowBank"),calibration,Source("ToneMapBank"));
        var converted=result.Files.ToDictionary(f=>f.Path,f=>f.Bytes,StringComparer.Ordinal);
        result.Files.RemoveAll(f=>f.Path.Contains("::",StringComparison.Ordinal));
        foreach(var (path,binder) in binders){foreach(var file in binder.Files)file.Bytes=DCX.Decompress(converted[path+"::"+file.Name]);result.Files.Add(new(path,binder.Write(DCX.Type.DCX_DFLT_10000_44_9)));}
        return result;
    }
    internal static EnvironmentSky BakeSky(FLVER2 source,FLVER2 native,Func<string,MTD> sourceDefinition,Func<string,MTD> nativeDefinition,Func<string,string> texturePath,string materialPrefix) {
        if(!Regex.IsMatch(materialPrefix,@"\A[A-Za-z0-9_]{1,48}\z"))throw new ArgumentException("Invalid imported sky material prefix");
        if(native.Header.Version is not(0x20013 or 0x20014)||native.Header.BigEndian)throw new InvalidDataException("Choose a Bloodborne native sky template");
        var result=FLVER2.Read(native.Write());result.Materials.Clear();result.Meshes.Clear();result.Dummies.Clear();result.Nodes=[new(){Name="root",ParentIndex=-1,Scale=Vector3.One}];
        var geometry=new StaticMapGeometry(source,"DS1R");var definitions=new List<EnvironmentMaterial>();var notes=new List<string>();int total=0;
        foreach(var mesh in source.Meshes) {
            var sourceMaterial=source.Materials[mesh.MaterialIndex];bool cloud=sourceMaterial.MTD.Contains("cloud",StringComparison.OrdinalIgnoreCase),dome=sourceMaterial.MTD.Contains("_Sky[",StringComparison.OrdinalIgnoreCase),distant=sourceMaterial.MTD.Contains("_m9000_M[D]",StringComparison.OrdinalIgnoreCase);
            if(!cloud&&!dome&&!distant){notes.Add("Non-sky source mesh omitted: "+sourceMaterial.MTD);continue;}
            if((long)total+mesh.Vertices.Count>100000)throw new InvalidDataException("Source sky exceeds the geometry budget");
            if(distant)notes.Add("Source distant landscape geometry and diffuse colors retained with a native unlit sky material; its source lighting response is approximated.");
            var template=native.Meshes.FirstOrDefault(m=>native.Materials[m.MaterialIndex].MTD.EndsWith(cloud?"M_Sky_daytime_[Dn]_Alp.mtd":"M_Sky_daytime_[Dn].mtd",StringComparison.OrdinalIgnoreCase))??
                native.Meshes.FirstOrDefault(m=>native.Materials[m.MaterialIndex].MTD.EndsWith(cloud?"M_Sky_Evening_[Dn]_Alp.mtd":"M_Sky_daytime_[Dn].mtd",StringComparison.OrdinalIgnoreCase))??throw new InvalidDataException("Native sky template material unavailable");
            var material=native.Materials[template.MaterialIndex];var definition=MTD.Read(nativeDefinition(material.MTD).Write());var from=sourceDefinition(sourceMaterial.MTD);
            var rgb=from.Params.Single(p=>p.Name=="g_DiffuseMapColor").Value as float[]??throw new InvalidDataException("Invalid source sky color");
            var power=System.Convert.ToSingle(from.Params.Single(p=>p.Name=="g_DiffuseMapColorPower").Value,CultureInfo.InvariantCulture);
            if(rgb.Length!=3||rgb.Any(f=>!float.IsFinite(f))||!float.IsFinite(power)||power<0)throw new InvalidDataException("Invalid source sky color/power");
            definition.Params.Single(p=>p.Name=="g_DiffuseColor").Value=rgb.ToArray();definition.Params.Single(p=>p.Name=="g_DiffuseColorPower").Value=power;
            string name=materialPrefix+"_sky_"+definitions.Count.ToString("D2")+".mtd";definitions.Add(new(name,definition));
            var diffuse=sourceMaterial.Textures.FirstOrDefault(t=>t.ParamName is "g_Diffuse" or "g_DiffuseTexture")??throw new InvalidDataException("Source sky diffuse texture unavailable");
            var outputMaterial=new FLVER2.Material("ImportedSky_"+result.Materials.Count,@"N:\SPRJ\data\Material\mtd\map\BBImport\"+name,material.Index){GXIndex=material.GXIndex,Textures=material.Textures.Select(t=>new FLVER2.Texture(t.ParamName,t.ParamName=="g_DiffuseTexture"?texturePath(diffuse.Path):t.Path,t.TilingScale,t.TilingTypeU,t.TilingTypeV,t.Unk14,t.Unk18,t.Unk1C)).ToList()};
            var outputMesh=new FLVER2.Mesh{UseBoneWeights=false,NodeIndex=0,MaterialIndex=result.Materials.Count};foreach(var buffer in template.VertexBuffers)outputMesh.VertexBuffers.Add(new(buffer.LayoutIndex){BufferIndex=buffer.BufferIndex});
            foreach(var vertex in mesh.Vertices)outputMesh.Vertices.Add(geometry.Bake(mesh,vertex,true));
            foreach(var face in StaticMapGeometry.Faces(source,mesh)){if(face.Indices.Count%3!=0||face.Indices.Any(i=>i<0||i>=outputMesh.Vertices.Count))throw new InvalidDataException("Invalid source sky triangles");outputMesh.FaceSets.Add(new(FLVER2.FaceSet.FSFlags.None,false,face.Cull,face.Unknown,face.Indices));}
            if(outputMesh.Vertices.Count==0||outputMesh.FaceSets.Count==0)throw new InvalidDataException("Source sky mesh has no geometry");
            outputMesh.BoundingBox=new(){Min=outputMesh.Vertices.Select(v=>v.Position).Aggregate(Vector3.Min),Max=outputMesh.Vertices.Select(v=>v.Position).Aggregate(Vector3.Max)};
            result.Materials.Add(outputMaterial);result.Meshes.Add(outputMesh);total+=outputMesh.Vertices.Count;
        }
        if(result.Meshes.Count==0)throw new InvalidDataException("No supported source sky or cloud meshes");
        result.Header.BoundingBoxMin=result.Meshes.Select(m=>m.BoundingBox.Min).Aggregate(Vector3.Min);result.Header.BoundingBoxMax=result.Meshes.Select(m=>m.BoundingBox.Max).Aggregate(Vector3.Max);result.Nodes[0].BoundingBoxMin=result.Header.BoundingBoxMin;result.Nodes[0].BoundingBoxMax=result.Header.BoundingBoxMax;
        notes.Add("Source sky/cloud diffuse textures, vertex RGBA and color multipliers retained using native compiled unlit/alpha sky shaders; source cloud normal lighting, UV scrolling and exact source sky scattering are not reproduced.");
        notes.Add("All sky meshes are non-solid; preserve the source MSB part transform plus map offset and use an independent visual EntityID rather than native time-phase entity IDs.");
        return new(result,definitions,notes);
    }
}
