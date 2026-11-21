// File:        UIComboBox_script.cpp
// Description: exports CUIComobBox to LUA environment
// Created:     11.12.2004
// Author:      Serhiy O. Vynnychenko
// Mail:        narrator@gsc-game.kiev.ua
//
// Copyright 2004 GSC Game World
//

#include "pch_script.h"
#include "UIComboBox.h"
#include "UIListBoxItem.h"

using namespace luabind;

#pragma optimize("s",on)
void CUIComboBox::script_register(lua_State* L)
{
	struct GamepadHelpers
	{
		static bool IsExpanded(CUIComboBox* box)
		{
			return box->m_list_frame.IsShown();
		}
		static void ShowList(CUIComboBox* box, bool show)
		{
			if (!box || !box->GetParent())
				return;
			if (box->m_list_frame.IsShown() != show)
				box->ShowList(show);
		}
		static CUIScrollView* List(CUIComboBox* box)
		{
			return static_cast<CUIScrollView*>(&box->m_list_box);
		}
		static CUIWindow* ListItem(CUIComboBox* box, int i)
		{
			return box->m_list_box.GetItemByIDX(i);
		}
		static void Select(CUIComboBox* box, int i)
		{
			if (!box->m_list_frame.IsShown())
				return;
			auto* item = box->m_list_box.GetItemByIDX(i);
			if (!item || !item->IsShown() || !item->IsEnabled())
				return;
			box->m_list_box.SetSelected(item);
			box->SendMessage(&box->m_list_box, LIST_ITEM_CLICKED);
		}
	};

	module(L)
	[
		class_<CUIComboBox, CUIWindow>("CUIComboBox")
		.def(constructor<>())
		.def("SetVertScroll", &CUIComboBox::SetVertScroll)
		.def("SetListLength", &CUIComboBox::SetListLength)
		.def("CurrentID", &CUIComboBox::CurrentID)
		.def("disable_id", &CUIComboBox::disable_id)
		.def("enable_id", &CUIComboBox::enable_id)
		.def("AddItem", &CUIComboBox::AddItem_)
		.def("GetText", &CUIComboBox::GetText)
		.def("GetTextOf", &CUIComboBox::GetTextOf)
		.def("SetText", &CUIComboBox::SetText)
		.def("ClearList", &CUIComboBox::ClearList)
		.def("SetCurrentOptValue", &CUIComboBox::SetCurrentOptValue)
		.def("SetCurrentIdx", &CUIComboBox::SetSelectedIDX)
		.def("GetCurrentIdx", &CUIComboBox::GetSelectedIDX)
		.def("GamepadIsExpanded", &GamepadHelpers::IsExpanded)
		.def("GamepadShowList", &GamepadHelpers::ShowList)
		.def("GamepadList", &GamepadHelpers::List)
		.def("GamepadListItem", &GamepadHelpers::ListItem)
		.def("GamepadSelect", &GamepadHelpers::Select)
	];
}
