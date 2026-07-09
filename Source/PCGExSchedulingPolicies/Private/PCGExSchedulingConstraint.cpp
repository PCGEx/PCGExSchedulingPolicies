// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "PCGExSchedulingConstraint.h"

#include "UObject/UnrealType.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(PCGExSchedulingConstraint)

bool UPCGExSchedulingConstraint::IsEquivalentTo(const UPCGExSchedulingConstraint* InOther) const
{
	if (this == InOther)
	{
		return true;
	}

	if (!InOther || InOther->GetClass() != GetClass())
	{
		return false;
	}

	for (TFieldIterator<FProperty> It(GetClass()); It; ++It)
	{
		const FProperty* Property = *It;

		if (Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
		{
			continue;
		}

		if (!Property->Identical_InContainer(this, InOther))
		{
			return false;
		}
	}

	return true;
}
