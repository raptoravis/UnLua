require "UnLua"

_G.ProjectileInitializer = ProjectileInitializer or {}

function ProjectileInitializer:GetRandomColor()
    local R = UE4.UKismetMathLibrary.RandomFloat()
    local G = UE4.UKismetMathLibrary.RandomFloat()
    local B = UE4.UKismetMathLibrary.RandomFloat()
    return UE4.FLinearColor(R, G, B, 1.0)
end

function ProjectileInitializer:GetInitializer()
    local color = self:GetRandomColor()
    return {
        Color = color,
        Ratio = (color.R + color.G + color.B) / 3,
    }
end

return ProjectileInitializer