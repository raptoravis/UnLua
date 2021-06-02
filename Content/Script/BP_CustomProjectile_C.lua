--
-- DESCRIPTION
--
-- @COMPANY **
-- @AUTHOR **
-- @DATE ${date} ${time}
--

require "UnLua"

--http://utmhikari.top/2020/08/02/geekdaily/unlua_try/

local BP_CustomProjectile_C = Class("Weapon.BP_ProjectileBase_C")

function BP_CustomProjectile_C:Initialize(Initializer)
    self.BaseColor = Initializer.Color
    self.DamageRatio = Initializer.Ratio or 0.1
end

function BP_CustomProjectile_C:UserConstructionScript()
    self.Super.UserConstructionScript(self)
    self.Damage = self.Damage * self.DamageRatio
    print("Projectile Damage: " .. tostring(self.Damage));
    self.DamageType = UE4.UClass.Load("/Game/Core/Blueprints/BP_DamageType.BP_DamageType_C")
end

function BP_CustomProjectile_C:ReceiveBeginPlay()
    self.Super.ReceiveBeginPlay(self)
    local MID = self.StaticMesh:CreateDynamicMaterialInstance(0)
    if MID then
        MID:SetVectorParameterValue("BaseColor", self.BaseColor)
    end
end

return BP_CustomProjectile_C