#pragma once


#include "UIStatic.h"
#include "../../xrServerEntities/script_export_space.h"

class CUI3tButton;
class CUIEditBox;

class CUIMessageBox : public CUIStatic
{
private:
	typedef CUIStatic inherited;
public:
	CUIMessageBox();
	virtual ~CUIMessageBox();

	//разновидности MessageBox
	typedef enum
	{
		MESSAGEBOX_OK,
		MESSAGEBOX_INFO,
		MESSAGEBOX_YES_NO,
		MESSAGEBOX_YES_NO_CANCEL,
		MESSAGEBOX_DIRECT_IP,
		MESSAGEBOX_PASSWORD,
		MESSAGEBOX_RA_LOGIN,
		MESSAGEBOX_QUIT_WINDOWS,
		MESSAGEBOX_QUIT_GAME,
		MESSAGEBOX_YES_NO_COPY
	} E_MESSAGEBOX_STYLE;

	virtual void InitMessageBox(LPCSTR xml_template);
	void Clear();
	virtual void SetText(LPCSTR str);
	virtual LPCSTR GetText();
	LPCSTR GetHost();
	LPCSTR GetPassword();
	LPCSTR GetUserPassword();
	void SetUserPasswordMode(bool);
	void SetPasswordMode(bool);
	E_MESSAGEBOX_STYLE GetBoxStyle() { return m_eMessageBoxStyle; };

	void SetTextEditURL(LPCSTR text);
	LPCSTR GetTextEditURL();

	virtual bool OnMouseAction(float x, float y, EUIMessages mouse_action);
	virtual void SendMessage(CUIWindow* pWnd, s16 msg, void* pData);

	void OnYesOk();
	virtual CUIWindow* ui_cast_window() { return this; }
	virtual CUIStatic* ui_cast_static() { return this; }

	bool HasGamepadConfirmation();
	bool HasGamepadOk(); // INV-REWORK-22: окно с одной кнопкой OK (значок A)
	bool ActivateGamepadConfirmation(bool yes);
	bool ActivateGamepadQuit(bool yes); // FIX-LIVE3 п.24: «Выход» — ОК напрямую через OnYesOk, иначе «Отмена»
	bool ActivateGamepadOk();
	bool IsGamepadDangerousConfirmation() const;
	bool IsGamepadQuitConfirmation() const; // FIX-LIVE2 п.34: выход в Windows / в меню — одно «ОК»
	bool HasGamepadFatalErrorChoice() const;
	bool ActivateGamepadFatalChoice(int choice);
	void SetGamepadHoldState(int targetChoice, float progress, bool done);
	void TriggerGamepadDenyShake(int targetChoice);
	int GetGamepadHoldChoice() const { return m_gpHoldChoice; }
	float GetGamepadHoldProgress() const { return m_gpHoldProgress; }
	bool IsGamepadHoldDone() const { return m_gpHoldDone; }
	u32 GetGamepadHoldDoneAt() const { return m_gpHoldDoneAt; }
	float SampleGamepadShakeOffset(int targetChoice) const;
	CUI3tButton* GetGamepadChoiceButton(int choice) const;
protected:
	xr_string m_ret_val;
	shared_str m_boxTemplate;
	int m_gpHoldChoice = -1;
	float m_gpHoldProgress = 0.0f;
	bool m_gpHoldDone = false;
	u32 m_gpHoldDoneAt = 0;
	int m_gpShakeChoice = -1;
	u32 m_gpShakeAt = 0;
	CUI3tButton* m_UIButtonYesOk;
	CUI3tButton* m_UIButtonNo;
	CUI3tButton* m_UIButtonCancel;
	CUI3tButton* m_UIButtonCopy;

	CUIStatic* m_UIStaticPicture;
	CUITextWnd* m_UIStaticText;
	CUITextWnd* m_UIStaticHost;
	CUITextWnd* m_UIStaticPass;
	CUITextWnd* m_UIStaticUserPass;
	CUIEditBox* m_UIEditHost;
	CUIEditBox* m_UIEditPass;
	CUIEditBox* m_UIEditUserPass;
	CUIEditBox* m_UIEditURL;

	E_MESSAGEBOX_STYLE m_eMessageBoxStyle = MESSAGEBOX_INFO;
DECLARE_SCRIPT_REGISTER_FUNCTION
};
