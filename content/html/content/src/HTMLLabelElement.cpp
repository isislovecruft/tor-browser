/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * Implementation of HTML <label> elements.
 */
#include "HTMLLabelElement.h"
#include "mozilla/dom/HTMLLabelElementBinding.h"
#include "nsEventDispatcher.h"
#include "nsFocusManager.h"

// construction, destruction

NS_IMPL_NS_NEW_HTML_ELEMENT(Label)

namespace mozilla {
namespace dom {

HTMLLabelElement::~HTMLLabelElement()
{
}

JSObject*
HTMLLabelElement::WrapNode(JSContext *aCx, JS::Handle<JSObject*> aScope)
{
  return HTMLLabelElementBinding::Wrap(aCx, aScope, this);
}

// nsISupports


NS_IMPL_ADDREF_INHERITED(HTMLLabelElement, Element)
NS_IMPL_RELEASE_INHERITED(HTMLLabelElement, Element)

// QueryInterface implementation for HTMLLabelElement
NS_INTERFACE_TABLE_HEAD(HTMLLabelElement)
  NS_HTML_CONTENT_INTERFACES(nsGenericHTMLFormElement)
  NS_INTERFACE_TABLE_INHERITED1(HTMLLabelElement,
                                nsIDOMHTMLLabelElement)
  NS_INTERFACE_TABLE_TO_MAP_SEGUE
NS_ELEMENT_INTERFACE_MAP_END


// nsIDOMHTMLLabelElement

NS_IMPL_ELEMENT_CLONE(HTMLLabelElement)

NS_IMETHODIMP
HTMLLabelElement::GetForm(nsIDOMHTMLFormElement** aForm)
{
  return nsGenericHTMLFormElement::GetForm(aForm);
}

NS_IMETHODIMP
HTMLLabelElement::GetControl(nsIDOMHTMLElement** aElement)
{
  nsCOMPtr<nsIDOMHTMLElement> element = do_QueryInterface(GetLabeledElement());
  element.forget(aElement);
  return NS_OK;
}

NS_IMETHODIMP
HTMLLabelElement::SetHtmlFor(const nsAString& aHtmlFor)
{
  ErrorResult rv;
  SetHtmlFor(aHtmlFor, rv);
  return rv.ErrorCode();
}

NS_IMETHODIMP
HTMLLabelElement::GetHtmlFor(nsAString& aHtmlFor)
{
  nsString htmlFor;
  GetHtmlFor(htmlFor);
  aHtmlFor = htmlFor;
  return NS_OK;
}

void
HTMLLabelElement::Focus(ErrorResult& aError)
{
  // retarget the focus method at the for content
  nsIFocusManager* fm = nsFocusManager::GetFocusManager();
  if (fm) {
    nsCOMPtr<nsIDOMElement> elem = do_QueryInterface(GetLabeledElement());
    if (elem)
      fm->SetFocus(elem, 0);
  }
}

static bool
EventTargetIn(nsEvent *aEvent, nsIContent *aChild, nsIContent *aStop)
{
  nsCOMPtr<nsIContent> c = do_QueryInterface(aEvent->target);
  nsIContent *content = c;
  while (content) {
    if (content == aChild) {
      return true;
    }

    if (content == aStop) {
      break;
    }

    content = content->GetParent();
  }
  return false;
}

static void
DestroyMouseDownPoint(void *    /*aObject*/,
                      nsIAtom * /*aPropertyName*/,
                      void *    aPropertyValue,
                      void *    /*aData*/)
{
  nsIntPoint* pt = static_cast<nsIntPoint*>(aPropertyValue);
  delete pt;
}

nsresult
HTMLLabelElement::PostHandleEvent(nsEventChainPostVisitor& aVisitor)
{
  if (mHandlingEvent ||
      (!NS_IS_MOUSE_LEFT_CLICK(aVisitor.mEvent) &&
       aVisitor.mEvent->message != NS_MOUSE_BUTTON_DOWN) ||
      aVisitor.mEventStatus == nsEventStatus_eConsumeNoDefault ||
      !aVisitor.mPresContext ||
      // Don't handle the event if it's already been handled by another label
      aVisitor.mEvent->mFlags.mMultipleActionsPrevented) {
    return NS_OK;
  }

  // Strong ref because event dispatch is going to happen.
  nsRefPtr<Element> content = GetLabeledElement();

  if (content && !EventTargetIn(aVisitor.mEvent, content, this)) {
    mHandlingEvent = true;
    switch (aVisitor.mEvent->message) {
      case NS_MOUSE_BUTTON_DOWN:
        NS_ASSERTION(aVisitor.mEvent->eventStructType == NS_MOUSE_EVENT,
                     "wrong event struct for event");
        if (static_cast<nsMouseEvent*>(aVisitor.mEvent)->button ==
            nsMouseEvent::eLeftButton) {
          // We reset the mouse-down point on every event because there is
          // no guarantee we will reach the NS_MOUSE_CLICK code below.
          nsIntPoint *curPoint = new nsIntPoint(aVisitor.mEvent->refPoint);
          SetProperty(nsGkAtoms::labelMouseDownPtProperty,
                      static_cast<void *>(curPoint),
                      DestroyMouseDownPoint);
        }
        break;

      case NS_MOUSE_CLICK:
        if (NS_IS_MOUSE_LEFT_CLICK(aVisitor.mEvent)) {
          const nsMouseEvent* event =
            static_cast<const nsMouseEvent*>(aVisitor.mEvent);
          nsIntPoint *mouseDownPoint = static_cast<nsIntPoint *>
            (GetProperty(nsGkAtoms::labelMouseDownPtProperty));

          bool dragSelect = false;
          if (mouseDownPoint) {
            nsIntPoint dragDistance = *mouseDownPoint;
            DeleteProperty(nsGkAtoms::labelMouseDownPtProperty);

            dragDistance -= aVisitor.mEvent->refPoint;
            const int CLICK_DISTANCE = 2;
            dragSelect = dragDistance.x > CLICK_DISTANCE ||
                         dragDistance.x < -CLICK_DISTANCE ||
                         dragDistance.y > CLICK_DISTANCE ||
                         dragDistance.y < -CLICK_DISTANCE;
          }
          // Don't click the for-content if we did drag-select text or if we
          // have a kbd modifier (which adjusts a selection).
          if (dragSelect || event->IsShift() || event->IsControl() ||
              event->IsAlt() || event->IsMeta()) {
            break;
          }
          // Only set focus on the first click of multiple clicks to prevent
          // to prevent immediate de-focus.
          if (event->clickCount <= 1) {
            nsIFocusManager* fm = nsFocusManager::GetFocusManager();
            if (fm) {
              // Use FLAG_BYMOVEFOCUS here so that the label is scrolled to.
              // Also, within HTMLInputElement::PostHandleEvent, inputs will
              // be selected only when focused via a key or when the navigation
              // flag is used and we want to select the text on label clicks as
              // well.
              nsCOMPtr<nsIDOMElement> elem = do_QueryInterface(content);
              fm->SetFocus(elem, nsIFocusManager::FLAG_BYMOVEFOCUS);
            }
          }
          // Dispatch a new click event to |content|
          //    (For compatibility with IE, we do only left click.  If
          //    we wanted to interpret the HTML spec very narrowly, we
          //    would do nothing.  If we wanted to do something
          //    sensible, we might send more events through like
          //    this.)  See bug 7554, bug 49897, and bug 96813.
          nsEventStatus status = aVisitor.mEventStatus;
          // Ok to use aVisitor.mEvent as parameter because DispatchClickEvent
          // will actually create a new event.
          widget::EventFlags eventFlags;
          eventFlags.mMultipleActionsPrevented = true;
          DispatchClickEvent(aVisitor.mPresContext,
                             static_cast<nsInputEvent*>(aVisitor.mEvent),
                             content, false, &eventFlags, &status);
          // Do we care about the status this returned?  I don't think we do...
          // Don't run another <label> off of this click
          aVisitor.mEvent->mFlags.mMultipleActionsPrevented = true;
        }
        break;
    }
    mHandlingEvent = false;
  }
  return NS_OK;
}

nsresult
HTMLLabelElement::Reset()
{
  return NS_OK;
}

NS_IMETHODIMP
HTMLLabelElement::SubmitNamesValues(nsFormSubmission* aFormSubmission)
{
  return NS_OK;
}

void
HTMLLabelElement::PerformAccesskey(bool aKeyCausesActivation,
                                   bool aIsTrustedEvent)
{
  if (!aKeyCausesActivation) {
    nsRefPtr<Element> element = GetLabeledElement();
    if (element)
      element->PerformAccesskey(aKeyCausesActivation, aIsTrustedEvent);
  } else {
    nsPresContext *presContext = GetPresContext();
    if (!presContext)
      return;

    // Click on it if the users prefs indicate to do so.
    nsMouseEvent event(aIsTrustedEvent, NS_MOUSE_CLICK,
                       nullptr, nsMouseEvent::eReal);
    event.inputSource = nsIDOMMouseEvent::MOZ_SOURCE_KEYBOARD;

    nsAutoPopupStatePusher popupStatePusher(aIsTrustedEvent ?
                                            openAllowed : openAbused);

    nsEventDispatcher::Dispatch(static_cast<nsIContent*>(this), presContext,
                                &event);
  }
}

nsGenericHTMLElement*
HTMLLabelElement::GetLabeledElement() const
{
  nsAutoString elementId;

  if (!GetAttr(kNameSpaceID_None, nsGkAtoms::_for, elementId)) {
    // No @for, so we are a label for our first form control element.
    // Do a depth-first traversal to look for the first form control element.
    return GetFirstLabelableDescendant();
  }

  // We have a @for. The id has to be linked to an element in the same document
  // and this element should be a labelable form control.
  nsIDocument* doc = GetCurrentDoc();
  if (!doc) {
    return nullptr;
  }

  Element* element = doc->GetElementById(elementId);
  if (element && element->IsLabelable()) {
    return static_cast<nsGenericHTMLElement*>(element);
  }

  return nullptr;
}

nsGenericHTMLElement*
HTMLLabelElement::GetFirstLabelableDescendant() const
{
  for (nsIContent* cur = nsINode::GetFirstChild(); cur;
       cur = cur->GetNextNode(this)) {
    Element* element = cur->IsElement() ? cur->AsElement() : nullptr;
    if (element && element->IsLabelable()) {
      return static_cast<nsGenericHTMLElement*>(element);
    }
  }

  return nullptr;
}

} // namespace dom
} // namespace mozilla
